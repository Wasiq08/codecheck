#include <core/debug.h>
#include <core/types.h>
#include <core/error.h>
#include <core/compiler.h>
#include <core/cpu.h>
#include <mm/xmalloc.h>
#include <mm/mm.h>
#include <fs/vfs.h>
#include <mm/page.h>
#include <core/module.h>
#include <lib/string.h>
#include <lib/ds.h>
#include <core/stdio.h>
#include <core/debug.h>
#include <core/size.h>
#include <core/debug.h>
#include <core/klogger.h>
#include <core/cache.h>
#include <core/random.h>
#include <mm/mm_locking.h>
#include <trustware/vm.h>
#include <cinttypes>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <mm/mm_cache.h>
#include <core/wsm.h>
#include <trustware/AutoDeleter.h>
#include <core/machine_desc.h>
#include <trustware/internal/vm_private.h>
#include <lib/OpenHashTable.h>
#include <mm/mm_mapper.h>
#include <vfs/fd.h>
#include <mm/mm_resource.h>
#include <lib/syscalls.h>
#include <debug/unwind.h>
#include <mm/mm_low_mem_framework.h>

ObjectCache * gPageMappingsObjectCache;

static unsigned int sNumPageFaults;

static int unmap_address_range(VMAddressSpace* addressSpace, uintptr_t address, size_t size,
	bool kernel);

static void fix_protection(uint32_t * protection)
{
	if ((*protection & B_KERNEL_PROTECTION) == 0) {
		if ((*protection & B_USER_PROTECTION) == 0
			|| (*protection & B_WRITE_AREA) != 0)
			*protection |= B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA;
		else
			*protection |= B_KERNEL_READ_AREA;
	}
}

class AreaCacheLocking {
public:
	inline bool Lock(VMCache*)
	{
		return false;
	}

	inline void Unlock(VMCache* lockable)
	{
		vm_area_put_locked_cache(lockable);
	}
};

class AreaCacheLocker : public AutoLocker<VMCache, AreaCacheLocking> {
public:
	inline AreaCacheLocker(VMCache* cache = nullptr)
		: AutoLocker<VMCache, AreaCacheLocking>(cache, true)
	{
	}

	inline AreaCacheLocker(VMArea* area)
		: AutoLocker<VMCache, AreaCacheLocking>()
	{
		SetTo(area);
	}

	inline void SetTo(VMCache* cache, bool alreadyLocked)
	{
		AutoLocker<VMCache, AreaCacheLocking>::SetTo(cache, alreadyLocked);
	}

	inline void SetTo(VMArea* area)
	{
		return AutoLocker<VMCache, AreaCacheLocking>::SetTo(
			area != nullptr ? vm_area_get_locked_cache(area) : nullptr, true, true);
	}
};

template<typename LockerType1, typename LockerType2>
static inline bool
wait_if_area_is_wired(VMArea* area, LockerType1* locker1, LockerType2* locker2)
{
	VMAreaUnwiredWaiter waiter;
	if (!area->AddWaiterIfWired(&waiter))
		return false;

	// unlock everything and wait
	if (locker1 != nullptr)
		locker1->Unlock();
	if (locker2 != nullptr)
		locker2->Unlock();

	waiter.waitEntry.Wait();

	return true;
}

template<typename LockerType1, typename LockerType2>
static inline bool
wait_if_area_range_is_wired(VMArea* area, uintptr_t base, size_t size,
	LockerType1* locker1, LockerType2* locker2)
{
	VMAreaUnwiredWaiter waiter;
	if (!area->AddWaiterIfWired(&waiter, base, size))
		return false;

	// unlock everything and wait
	if (locker1 != nullptr)
		locker1->Unlock();
	if (locker2 != nullptr)
		locker2->Unlock();

	waiter.waitEntry.Wait();

	return true;
}

template<typename LockerType>
static inline bool
wait_if_address_range_is_wired(VMAddressSpace* addressSpace, uintptr_t base,
	size_t size, LockerType* locker)
{
	for (VMAddressSpace::AreaRangeIterator it =
			addressSpace->GetAreaRangeIterator(base, size);
			VMArea *area = it.Next();) {
		AreaCacheLocker cacheLocker(vm_area_get_locked_cache(area));

		if (wait_if_area_range_is_wired(area, base, size, locker, &cacheLocker))
			return true;
	}

	return false;
}

class VMCacheChainLocker {
public:
	VMCacheChainLocker()
		:
		fTopCache(nullptr),
		fBottomCache(nullptr)
	{
	}

	VMCacheChainLocker(VMCache* topCache)
		:
		fTopCache(topCache),
		fBottomCache(topCache)
	{
	}

	~VMCacheChainLocker()
	{
		Unlock();
	}

	void SetTo(VMCache* topCache)
	{
		fTopCache = topCache;
		fBottomCache = topCache;

		if (topCache != nullptr)
			topCache->SetUserData(nullptr);
	}

	VMCache* LockSourceCache()
	{
		if (fBottomCache == nullptr || fBottomCache->source == nullptr)
			return nullptr;

		VMCache* previousCache = fBottomCache;

		fBottomCache = fBottomCache->source;
		fBottomCache->Lock();
		fBottomCache->AcquireRefLocked();
		fBottomCache->SetUserData(previousCache);

		return fBottomCache;
	}

	void LockAllSourceCaches()
	{
		while (LockSourceCache() != nullptr) {
		}
	}

	void Unlock(VMCache* exceptCache = nullptr)
	{
		if (fTopCache == nullptr)
			return;

		// Unlock caches in source -> consumer direction. This is important to
		// avoid double-locking and a reversal of locking order in case a cache
		// is eligable for merging.
		VMCache* cache = fBottomCache;
		while (cache != nullptr) {
			VMCache* nextCache = (VMCache*)cache->UserData();
			if (cache != exceptCache)
				cache->ReleaseRefAndUnlock(cache != fTopCache);

			if (cache == fTopCache)
				break;

			cache = nextCache;
		}

		fTopCache = nullptr;
		fBottomCache = nullptr;
	}

	void UnlockKeepRefs(bool keepTopCacheLocked)
	{
		if (fTopCache == nullptr)
			return;

		VMCache* nextCache = fBottomCache;
		VMCache* cache = nullptr;

		while (keepTopCacheLocked
				? nextCache != fTopCache : cache != fTopCache) {
			cache = nextCache;
			nextCache = (VMCache*)cache->UserData();
			cache->Unlock(cache != fTopCache);
		}
	}

	void RelockCaches(bool topCacheLocked)
	{
		if (fTopCache == nullptr)
			return;

		VMCache* nextCache = fTopCache;
		VMCache* cache = nullptr;
		if (topCacheLocked) {
			cache = nextCache;
			nextCache = cache->source;
		}

		while (cache != fBottomCache && nextCache != nullptr) {
			VMCache* consumer = cache;
			cache = nextCache;
			nextCache = cache->source;
			cache->Lock();
			cache->SetUserData(consumer);
		}
	}

private:
	VMCache*	fTopCache;
	VMCache*	fBottomCache;
};

static inline uintptr_t virtual_page_address(VMArea * area, struct page * page)
{
	return area->Base()
		+ ((page->cache_offset << PAGE_SHIFT) - area->cache_offset);
}

/*!	The page's cache must be locked.
*/
static inline void increment_page_wired_count(struct page * page)
{
	if (!page->IsMapped())
		__atomic_fetch_add(&gMappedPagesCount, 1, __ATOMIC_RELAXED);
	page->IncrementWiredCount();
}

/*!	The page's cache must be locked.
*/
static inline void decrement_page_wired_count(struct page * page)
{
	page->DecrementWiredCount();
	if (!page->IsMapped())
		__atomic_fetch_sub(&gMappedPagesCount, 1, __ATOMIC_RELAXED);
}

int allocate_area_page_protections(VMArea * area)
{
	// In the page protections we store only the three user protections,
	// so we use 4 bits per page.
	uint32_t bytes = (area->Size() / PAGE_SIZE + 1) / 2;
	area->page_protections = (uint8_t*)malloc_etc(bytes, CACHE_DONT_LOCK_KERNEL);
	if (area->page_protections == nullptr)
		return -ENOMEM;

	// init the page protections for all pages to that of the area
	uint32_t areaProtection = area->protection & (B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA);
	memset(area->page_protections, areaProtection | (areaProtection << 4), bytes);
	return 0;
}

static inline void set_area_page_protection(VMArea* area, uintptr_t pageAddress, uint32_t protection)
{
	protection &= B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA;
	uint32_t pageIndex = (pageAddress - area->Base()) / PAGE_SIZE;
	uint8_t& entry = area->page_protections[pageIndex / 2];
	if (pageIndex % 2 == 0)
		entry = (entry & 0xf0) | protection;
	else
		entry = (entry & 0x0f) | (protection << 4);
}

static inline vma_flags_t get_area_page_protection(VMArea* area, uintptr_t pageAddress)
{
	if (area->page_protections == nullptr)
		return area->protection;

	uint32_t pageIndex = (pageAddress - area->Base()) / PAGE_SIZE;
	uint32_t protection = area->page_protections[pageIndex / 2];
	if (pageIndex % 2 == 0)
		protection &= 0x0f;
	else
		protection >>= 4;

	// If this is a kernel area we translate the user flags to kernel flags.
	if (area->address_space == VMAddressSpace::Kernel()) {
		uint32_t kernelProtection = 0;
		if ((protection & B_READ_AREA) != 0)
			kernelProtection |= B_KERNEL_READ_AREA;
		if ((protection & B_WRITE_AREA) != 0)
			kernelProtection |= B_KERNEL_WRITE_AREA;

		return kernelProtection | (area->protection & B_KERNEL_MEMORY_TYPE_MASK);
	}

	return protection | B_KERNEL_READ_AREA
		| (protection & B_WRITE_AREA ? B_KERNEL_WRITE_AREA : 0)
		| (area->protection & B_KERNEL_MEMORY_TYPE_MASK);
}

void delete_area(VMAddressSpace* addressSpace, VMArea* area, bool deletingAddressSpace)
{
	uint32_t allocationFlags =
			addressSpace == VMAddressSpace::Kernel() ?
					CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL : 0;

	ASSERT(!area->IsWired());

	VMAreaHash::Remove(area);

	// Unmap the virtual address space the area occupied.
	{
		// We need to lock the complete cache chain.
		VMCache* topCache = vm_area_get_locked_cache(area);

		VMCacheChainLocker cacheChainLocker(topCache);
		cacheChainLocker.LockAllSourceCaches();

		bool ignoreTopCachePageFlags
			= topCache->temporary && topCache->RefCount() == 2;

		addressSpace->TranslationMap()->UnmapArea(area, deletingAddressSpace, ignoreTopCachePageFlags);
	}

	if (area->cache && !area->cache->temporary)
		area->cache->WriteModified();

	arch_vm_unset_memory_type(area);
	addressSpace->RemoveArea(area, allocationFlags);
	addressSpace->Put();

	if (area->cache) {
		area->cache->RemoveArea(area);
		area->cache->ReleaseRef();
	}

	addressSpace->DeleteArea(area, allocationFlags);
}

void vm_delete_areas(struct VMAddressSpace *addressSpace, bool deletingAddressSpace)
{
	addressSpace->WriteLock();

	// remove all reserved areas in this address space
	addressSpace->UnreserveAllAddressRanges(0);

	// delete all the areas in this address space
	while (VMArea* area = addressSpace->FirstArea()) {
		ASSERT(!area->IsWired());
		delete_area(addressSpace, area, deletingAddressSpace);
	}

	addressSpace->WriteUnlock();
}

static int map_page(struct VMArea * area, struct page * page, uintptr_t address, vma_flags_t protection, vm_reservation_t * reservation)
{
	VMTranslationMap* map = area->address_space->TranslationMap();

	bool wasMapped = page->IsMapped();

	if (area->wiring == B_NO_LOCK) {
		bool isKernelSpace = area->address_space == VMAddressSpace::Kernel();
		vm_page_mapping * mapping = (vm_page_mapping *) object_cache_alloc(
				gPageMappingsObjectCache,
				CACHE_DONT_WAIT_FOR_MEMORY | (isKernelSpace ? CACHE_DONT_LOCK_KERNEL : 0));

		if(!mapping)
			return -ENOMEM;

		mapping->page = page;
		mapping->area = area;

		map->Lock();
		map->Map(address, page_to_phys(page),
				(area->protection & B_KERNEL_MEMORY_TYPE_MASK)
						| (protection & ~B_KERNEL_MEMORY_TYPE_MASK),
				reservation);

		if(!page->IsMapped())
			__atomic_fetch_add(&gMappedPagesCount, 1, __ATOMIC_RELAXED);

		page->mappings.Add(mapping);
		area->mappings.Add(mapping);

		map->Unlock();
	} else {
		map->Lock();
		map->Map(address, page_to_phys(page),
				(area->protection & B_KERNEL_MEMORY_TYPE_MASK)
						| (protection & ~B_KERNEL_MEMORY_TYPE_MASK),
				reservation);
		map->Unlock();

		increment_page_wired_count(page);
	}

	if (!wasMapped) {
		// The page is mapped now, so we must not remain in the cached queue.
		// It also makes sense to move it from the inactive to the active, since
		// otherwise the page daemon wouldn't come to keep track of it (in idle
		// mode) -- if the page isn't touched, it will be deactivated after a
		// full iteration through the queue at the latest.
		if (page->state == PAGE_STATE_CACHED
				|| page->state == PAGE_STATE_INACTIVE) {
			vm_page_set_state(page, PAGE_STATE_ACTIVE);
		}
	}

	// Update minimal mapped address
	area->SetCommitBase(MIN(area->CommitBase(), address));

	return 0;
}

static inline int unmap_page(struct VMArea * area, uintptr_t virtualAddress)
{
	return area->address_space->TranslationMap()->UnmapPage(area, virtualAddress, true);
}

static inline void unmap_pages(struct VMArea * area, uintptr_t base, size_t size)
{
	area->address_space->TranslationMap()->UnmapPages(area, base, size, true);
}

static int map_backing_store(VMAddressSpace * addressSpace, VMCache* cache, off_t offset,
	const char* areaName, size_t size, uint32_t wiring, vma_flags_t protection,
	vma_flags_t protectionMax, uint32_t mapFlags, bool privateMapping,
	const virtual_address_restrictions* addressRestrictions,
	bool kernel, VMArea ** _area, vaddr_t * _virtualAddress, uint32_t allocationFlags)
{
	int status;

	cache->AssertLocked();

	int priority;
	if (addressSpace != VMAddressSpace::Kernel()) {
		priority = GFP_PRIO_USER;
	} else if ((mapFlags & CREATE_AREA_PRIORITY_VIP) != 0) {
		priority = GFP_PRIO_VIP;
		allocationFlags |= CACHE_USE_RESERVE;
	} else {
		priority = GFP_PRIO_SYSTEM;
	}

	VMArea* area = addressSpace->CreateArea(areaName, wiring, protection,
		allocationFlags);

	if (!area) {
		return -ENOMEM;
	}

	if (!privateMapping) {
		area->protection_max = protectionMax & B_USER_PROTECTION;
	}

	size = roundup2(size, PAGE_SIZE);

	area->cache_type = cache->type;

	// if this is a private map, we need to create a new cache
	// to handle the private copies of pages as they are written to
	VMCache* sourceCache = cache;
	if (privateMapping) {
		VMCache* newCache;

		// create an anonymous cache
		status = VMCacheFactory::CreateAnonymousCache(newCache,
			(protection & (B_STACK_AREA | B_KERNEL_STACK_AREA)) != 0
			|| (protection & B_OVERCOMMITTING_AREA) != 0, 0,
			cache->GuardSize() / PAGE_SIZE, true, GFP_PRIO_USER, allocationFlags);

		if (status != 0)
			goto err1;

		newCache->Lock();
		newCache->temporary = true;
		newCache->virtual_base = offset;
		newCache->virtual_end = offset + size;

		cache->AddConsumer(newCache);

		cache = newCache;
	}

	if ((mapFlags & CREATE_AREA_DONT_COMMIT_MEMORY) == 0) {
		status = cache->SetMinimalCommitment(size, priority);
		if (status != 0) {
			goto err2;
		}
	}

	// check to see if this address space has entered DELETE state
	if (addressSpace->IsBeingDeleted()) {
		// okay, someone is trying to delete this address space now, so we can't
		// insert the area, so back out
		status = -ESRCH;
		goto err2;
	}

	if (addressRestrictions->address_specification == B_EXACT_ADDRESS &&
			(mapFlags & CREATE_AREA_UNMAP_ADDRESS_RANGE))
	{
		// Temporarily we need to drop the sourceCache lock
		// as this may create mutex lock conflict when remapping files
		// In case of privateMapping::newCache, it doesn't matter, as
		// this cache is fresh
		sourceCache->Unlock();

		status = unmap_address_range(addressSpace,
				(uintptr_t) addressRestrictions->address, size, kernel);

		sourceCache->Lock();
		if (status != 0)
			goto err2;
	}

	status = addressSpace->InsertArea(area, size, addressRestrictions,
		allocationFlags, (void **)_virtualAddress);

	if (status != 0) {
		goto err2;
	}

	// attach the cache to the area
	area->cache = cache;
	area->cache_offset = offset;
	area->SetCommitBase(area->Base() + area->Size());

	// point the cache back to the area
	cache->InsertAreaLocked(area);
	if (privateMapping) {
		cache->Unlock();
	}

	VMAreaHash::Insert(area);
	addressSpace->Get();

	*_area = area;
	return 0;

err2:
	if (privateMapping) {
		// We created this cache, so we must delete it again. Note, that we
		// need to temporarily unlock the source cache or we'll otherwise
		// deadlock, since VMCache::_RemoveConsumer() will try to lock it, too.
		sourceCache->Unlock();
		cache->ReleaseRefAndUnlock();
		sourceCache->Lock();
	}
err1:
	addressSpace->DeleteArea(area, allocationFlags);
	return status;
}

static inline bool intersect_area(VMArea *area, uintptr_t &address, size_t &size,
		uintptr_t &offset) {
	if (address < area->Base()) {
		offset = area->Base() - address;
		if (offset >= size)
			return false;

		address = area->Base();
		size -= offset;
		offset = 0;
		if (size > area->Size())
			size = area->Size();

		return true;
	}

	offset = address - area->Base();
	if (offset >= area->Size())
		return false;

	if (size >= area->Size() - offset)
		size = area->Size() - offset;

	return true;
}

/*!	Cuts a piece out of an area. If the given cut range covers the complete
	area, it is deleted. If it covers the beginning or the end, the area is
	resized accordingly. If the range covers some part in the middle of the
	area, it is split in two; in this case the second area is returned via
	\a _secondArea (the variable is left untouched in the other cases).
	The address space must be write locked.
	The caller must ensure that no part of the given range is wired.
*/
static int cut_area(VMAddressSpace * addressSpace, VMArea * area, uintptr_t address,
	size_t size, VMArea ** _secondArea, bool kernel)
{
	// Does the cut range intersect with the area at all?
	uintptr_t offset = 0;
	if (!intersect_area(area, address, size, offset))
		return 0;

	// Is the area fully covered?
	if (address == area->Base() && size == area->Size()) {
		delete_area(addressSpace, area, false);
		return 0;
	}

	int priority;

	int allocationFlags;
	if (addressSpace == VMAddressSpace::Kernel()) {
		priority = GFP_PRIO_SYSTEM;
		allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
	} else {
		priority = GFP_PRIO_USER;
		allocationFlags = 0;
	}

	VMCache* cache = vm_area_get_locked_cache(area);
	ASSERT(cache);
	VMCacheChainLocker cacheChainLocker(cache);
	cacheChainLocker.LockAllSourceCaches();

	// If no one else uses the area's cache and it's an anonymous cache, we can
	// resize or split it, too.
	bool onlyCacheUser = cache->areas == area
			&& area->cache_next == NULL && cache->consumers.IsEmpty()
			&& cache->type == CACHE_TYPE_RAM;

	// Cut the end only?
	if (offset > 0 && size == area->Size() - offset) {
		int error = addressSpace->ShrinkAreaTail(area, offset,
				allocationFlags);

		if (error != 0)
			return error;

		// unmap pages
		unmap_pages(area, address, size);

		// If no one else uses the area's cache, we can resize it, too.
		if (onlyCacheUser && !cache->virtual_cache) {
			// Since VMCache::Resize() can temporarily drop the lock, we must
			// unlock all lower caches to prevent locking order inversion.
			cacheChainLocker.Unlock(cache);
			cache->Resize(cache->virtual_base + offset,
					priority);
			cache->ReleaseRefAndUnlock();
		}

		return 0;
	}

	// Cut the beginning only?
    if (area->Base() == address) {
		// resize the area
		int error = addressSpace->ShrinkAreaHead(area, area->Size() - size,
                allocationFlags);

		if (error != 0)
			return error;

		// unmap pages
		unmap_pages(area, address, size);

		// If no one else uses the area's cache, we can resize it, too.
		if (onlyCacheUser && !cache->virtual_cache) {
			// Since VMCache::Rebase() can temporarily drop the lock, we must
			// unlock all lower caches to prevent locking order inversion.
			cacheChainLocker.Unlock(cache);
			cache->Rebase(cache->virtual_base + size, priority);
			cache->ReleaseRefAndUnlock();
		}

		area->cache_offset += size;
		return 0;
	}

	// The tough part -- cut a piece out of the middle of the area.
	// We do that by shrinking the area to the begin section and creating a
	// new area for the end section.

	uintptr_t firstNewSize = offset;
	uintptr_t secondBase = address + size;
	uintptr_t secondSize = area->Size() - offset - size;

	// unmap pages
	unmap_pages(area, address, area->Size() - firstNewSize);

	// resize the area
	uintptr_t oldSize = area->Size();
	int error = addressSpace->ShrinkAreaTail(area, firstNewSize, allocationFlags);
	if (error != 0)
		return error;

	/* Unmapping subset of vma range */
	VMArea* secondArea;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void*)secondBase;
	addressRestrictions.address_specification = B_EXACT_ADDRESS;

	if (onlyCacheUser && !cache->virtual_cache) {
		// Create a new cache for the second area.
		VMCache *secondCache;
		error = VMCacheFactory::CreateAnonymousCache(secondCache, false, 0, 0,
				dynamic_cast<VMAnonymousNoSwapCache*>(cache) == NULL, priority,
				allocationFlags);
		if (error != 0) {
			addressSpace->ShrinkAreaTail(area, oldSize, allocationFlags);
			return error;
		}

		secondCache->Lock();
		secondCache->temporary = cache->temporary;
		secondCache->virtual_base = area->cache_offset;
		secondCache->virtual_end = area->cache_offset + secondSize;

		// Transfer the concerned pages from the first cache.
		off_t adoptOffset = area->cache_offset + secondBase - area->Base();
		error = secondCache->Adopt(cache, adoptOffset, secondSize,
			area->cache_offset);

		if (error == 0) {
			// Since VMCache::Resize() can temporarily drop the lock, we must
			// unlock all lower caches to prevent locking order inversion.
			cacheChainLocker.Unlock(cache);
			cache->Resize(cache->virtual_base + firstNewSize, priority);
			// Don't unlock the cache yet because we might have to resize it
			// back.

			// Map the second area.
			error = map_backing_store(addressSpace, secondCache,
					area->cache_offset, area->name, secondSize, area->wiring,
					area->protection, area->protection_max, 0, false, &addressRestrictions, kernel,
					&secondArea, nullptr, allocationFlags);
		}

		if (error != 0) {
			// Restore the original cache.
			cache->Resize(cache->virtual_base + oldSize, priority);

			// Move the pages back.
			int readoptStatus = cache->Adopt(secondCache,
				area->cache_offset, secondSize, adoptOffset);

			if (readoptStatus != 0) {
				// Some (swap) pages have not been moved back and will be lost
				// once the second cache is deleted.
				printk(KERN_WARN, "failed to restore cache range: %d",
						readoptStatus);
				// TODO: Handle out of memory cases by freeing memory and
				// retrying.
			}

			cache->ReleaseRefAndUnlock();
			secondCache->ReleaseRefAndUnlock();
			addressSpace->ShrinkAreaTail(area, oldSize, allocationFlags);
			return error;
		}

		// Now we can unlock it.
		cache->ReleaseRefAndUnlock();
		secondCache->Unlock();
	} else {
		error = map_backing_store(addressSpace, cache,
				area->cache_offset + (secondBase - area->Base()), area->name,
				secondSize, area->wiring, area->protection, area->protection_max,
				0, false,
				&addressRestrictions, kernel, &secondArea, nullptr,
				allocationFlags);

		if (error != 0) {
			addressSpace->ShrinkAreaTail(area, oldSize, allocationFlags);
			return error;
		}

		cache->AcquireRefLocked();
	}

	if (_secondArea != nullptr)
		*_secondArea = secondArea;

	return 0;
}

/*!	Deletes all areas in the given address range.
	The address space must be write-locked.
	The caller must ensure that no part of the given range is wired.
*/
static int unmap_address_range(VMAddressSpace* addressSpace, uintptr_t address,
		size_t size, bool kernel) {
	size = roundup2(size, PAGE_SIZE);

	// Check, whether the caller is allowed to modify the concerned areas.
	if (!kernel) {
		for (VMAddressSpace::AreaRangeIterator it =
				addressSpace->GetAreaRangeIterator(address, size);
				VMArea *area = it.Next();) {

			if ((area->protection & B_KERNEL_AREA) != 0) {
				printk(KERN_ERR, "unmap_address_range: team %" PRId32 " tried to "
						"unmap range of kernel area %" PRId32 " (%s)\n",
						getpid(), area->id, area->name);
				return -EPERM;
			}
		}
	}

	for (VMAddressSpace::AreaRangeIterator it =
			addressSpace->GetAreaRangeIterator(address, size);
			VMArea *area = it.Next();) {
		int error = cut_area(addressSpace, area, address, size, nullptr,
				kernel);
		if (error != 0)
			return error;
		// Failing after already messing with areas is ugly, but we
		// can't do anything about it.

	}

	return 0;
}

int __sys_mprotect(void * _address, size_t size, uint32_t protection)
{
	uintptr_t address = (uintptr_t)_address;
	size = roundup2(size, PAGE_SIZE);

	if ((address % PAGE_SIZE) != 0)
		return -EINVAL;

	if ((uintptr_t)address + size < (uintptr_t)address ||
		!is_userspace_ptr_valid(_address, size))
	{
		// weird error code required by POSIX
		return -ENOMEM;
	}

	// extend and check protection
	if ((protection & ~B_USER_PROTECTION) != 0)
		return -EINVAL;

	fix_protection(&protection);

	// We need to write lock the address space, since we're going to play with
	// the areas. Also make sure that none of the areas is wired and that we're
	// actually allowed to change the protection.
	AddressSpaceWriteLocker locker;

	bool restart;
	do {
		restart = false;

		int status = locker.SetTo(VMAddressSpace::CurrentID());
		if (status != 0)
			return status;

		// First round: Check whether the whole range is covered by areas and we
		// are allowed to modify them.
		uintptr_t currentAddress = address;
		size_t sizeLeft = size;
		while (sizeLeft > 0) {
			VMArea* area = locker.AddressSpace()->LookupArea(currentAddress);

			if (area == nullptr)
				return -ENOMEM;

			if (!(area->protection
					& (B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA)))
				return -EPERM;

			if (area->protection & B_KERNEL_AREA)
				return -EPERM;

			if (area->protection_max != 0
					&& (protection & area->protection_max) != protection) {
				return -EPERM;
			}

			// Don't allow modification of NS area protections
			if ((area->protection
					& (VMA_FLAG_NONSECURE | VMA_FLAG_DEVICE
							| VMA_EFLAG_CACHE_MASK))
					&& (protection & PROT_EXECUTE))
				return -ENOMEM;

			uintptr_t offset = currentAddress - area->Base();
			size_t rangeSize = std::min<size_t>(area->Size() - offset, sizeLeft);

			AreaCacheLocker cacheLocker(area);

			if (wait_if_area_range_is_wired(area, currentAddress, rangeSize,
					&locker, &cacheLocker)) {
				restart = true;
				break;
			}

			currentAddress += rangeSize;
			sizeLeft -= rangeSize;
		}
	} while (restart);

	// Second round: If the protections differ from that of the area, create a
	// page protection array and re-map mapped pages.
	VMTranslationMap* map = locker.AddressSpace()->TranslationMap();
	uintptr_t currentAddress = address;
	size_t sizeLeft = size;
	while (sizeLeft > 0) {
		VMArea* area = locker.AddressSpace()->LookupArea(currentAddress);
		if (area == nullptr)
			return -ENOMEM;

		if ((area->protection & B_KERNEL_AREA) != 0)
			return -EPERM;

		uintptr_t offset = currentAddress - area->Base();
		size_t rangeSize = std::min<size_t>(area->Size() - offset, sizeLeft);

		currentAddress += rangeSize;
		sizeLeft -= rangeSize;

		if (area->page_protections == nullptr) {
			if ((area->protection & B_USER_PROTECTION) == (protection & B_USER_PROTECTION))
				continue;

			// Don't allow modification of NS area protections
			if((area->protection & (B_NONSECURE_AREA | B_DEVICE_AREA | B_MTR_CACHE_MASK)) && (protection & B_EXECUTE_AREA))
				return -ENOMEM;

			int status = allocate_area_page_protections(area);

			if (status != 0)
				return status;
		}

		// We need to lock the complete cache chain, since we potentially unmap
		// pages of lower caches.
		VMCache* topCache = vm_area_get_locked_cache(area);
		VMCacheChainLocker cacheChainLocker(topCache);
		cacheChainLocker.LockAllSourceCaches();

		if(area->cache_type >= CACHE_TYPE_DEVICE) {
			for (uintptr_t pageAddress = area->Base() + offset;
					pageAddress < currentAddress; pageAddress += PAGE_SIZE)
			{
				set_area_page_protection(area, pageAddress, protection);
			}
			map->Lock();
			map->Protect(area->Base() + offset, currentAddress - (area->Base() + offset), get_area_page_protection(area, area->Base() + offset));
			map->Unlock();
		} else {
			for (uintptr_t pageAddress = area->Base() + offset;
					pageAddress < currentAddress; pageAddress += PAGE_SIZE)
			{
				map->Lock();

				set_area_page_protection(area, pageAddress, protection);

				vm_paddr_t physicalAddress;
				uint32_t flags;

				int error = map->Query(pageAddress, &physicalAddress, &flags);
				if (error != 0 || (flags & PAGE_PRESENT) == 0) {
					map->Unlock();
					continue;
				}

				vm_page_t page = phys_to_page(physicalAddress);

				if (page == nullptr) {
					map->Unlock();
					panic("area %p looking up page failed for pa %#" PRIx64
						"\n", area, (uint64_t)physicalAddress);
				}

				bool unmapPage = page->Cache() != topCache
					&& (protection & B_WRITE_AREA) != 0;

				if (!unmapPage) {
					map->ProtectPage(area, pageAddress, protection);
				}

				map->Unlock();

				if(unmapPage) {
					unmap_page(area, pageAddress);
				}
			}
		}
	}

	return 0;
}

void* uva_map_pages(vm_paddr_t phyaddr,size_t size)
{
	char fmap_name[B_OS_NAME_LENGTH];
	vm_paddr_t phys_aligned = ROUND_PGDOWN_LPAE(phyaddr);
	vm_size_t size_aligned = ROUND_PGUP_LPAE(phyaddr + size) - phys_aligned;

	size_t blockSize =
			team_get_current_team()->addressSpace->TranslationMap()->HardwareBlockSize();

	snprintf(fmap_name, sizeof(fmap_name), "physmap %" PRIx64,
			(uint64_t) phyaddr);

	vaddr_t addr = 0;

	// Prefer alignment of allocation to size
	uint32_t addressSpec = (size >= blockSize) ?
			B_ANY_KERNEL_BLOCK_ADDRESS :
			B_RANDOMIZED_ANY_ADDRESS;

	if (team_get_current_team() == team_get_kernel_team()) {
		printk(KERN_ERR|WITHUART,
				"uva_map_pages called from kernel task context. Please fix your driver\n");
		unwind_issue_backtrace();
		return nullptr;
	}

	area_id id = vm_map_physical_memory(VMAddressSpace::CurrentID(), fmap_name,
			&addr, addressSpec, size_aligned,
			B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA
					| B_KERNEL_WRITE_AREA | B_MTR_CACHE_NC,
			phys_aligned,
			false);

	if (id < 0) {
		return nullptr;
	}

	dcache_inv_poc(addr, phys_aligned, size_aligned);

	return (void*) (addr + (phyaddr & (PAGE_SIZE - 1)));
}

int uvm_unmap_user_memory(uint32_t pid, uintptr_t address, size_t size)
{
	// check params
	if (size == 0 || (uintptr_t)address + size < (uintptr_t)address
		|| (uintptr_t)address % PAGE_SIZE != 0)
	{
		return -EINVAL;
	}

	if (!is_userspace_ptr_valid((const void *)address, size))
		return -EFAULT;

	if (pid == B_SYSTEM_TEAM) {
		printk(KERN_ERR|WITHUART,
				"uvm_unmap_user_memory called for kernel PID\n");
		unwind_issue_backtrace();
		return -EINVAL;
	}

	// Write lock the address space and ensure the address range is not wired.
	AddressSpaceWriteLocker locker;
	do {
		int status = locker.SetTo(pid);
		if (status != 0)
			return status;
	} while (wait_if_address_range_is_wired(locker.AddressSpace(), address,
			size, &locker));

	// unmap
	return unmap_address_range(locker.AddressSpace(), address, size, false);
}

int uva_unmap_pages(vaddr_t start,size_t size)
{
	size = roundup2(start + size, PAGE_SIZE) - rounddown2(start, PAGE_SIZE);
	start = rounddown2(start, PAGE_SIZE);

	if (team_get_current_team() == team_get_kernel_team()) {
		printk(KERN_ERR|WITHUART, "uva_unmap_pages called from kernel context. "
				"Please use uvm_unmap_user_memory with PID of memory owner\n");
		unwind_issue_backtrace();
		return -EINVAL;
	}

	return uvm_unmap_user_memory(VMAddressSpace::CurrentID(), start, size);
}

void* get_phy_to_vir(vm_paddr_t phyaddr, size_t size)
{
	return uva_map_pages(phyaddr, size);
}

int vm_map_physical_memory(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t size, vma_flags_t protection,
	vm_paddr_t physicalAddress, bool alreadyWired)
{
	VMArea* area;
	VMCache* cache;
	uintptr_t mapOffset;
	int allocationFlags;
	vm_reservation_t reservation;

	if (!arch_vm_supports_protection(protection))
		return -EINVAL;

	// create a device cache
	int status;

	if(team == (uint32_t)VMAddressSpace::KernelID()) {
		allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
	} else {
		allocationFlags = 0;
	}

	// if the physical address is somewhat inside a page,
	// move the actual area down to align on a page boundary
	mapOffset = physicalAddress % PAGE_SIZE;
	size += mapOffset;
	physicalAddress -= mapOffset;

	size = ROUND_PGUP(size);

	status = VMCacheFactory::CreateDeviceCache(cache, allocationFlags);

	if (status != 0)
		return status;

	if (!alreadyWired) {
		size_t reservePages =
				VMAddressSpace::Kernel()->TranslationMap()->MaxPagesNeededToMap(
						0, size);
		if (!vm_reserve_pages(&reservation, reservePages,
				team == (uint32_t) VMAddressSpace::KernelID() ?
						GFP_PRIO_SYSTEM : GFP_PRIO_USER,
						false,
						false))
		{
			cache->ReleaseRef();
			return -ENOMEM;
		}
	}

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked()) {
		cache->ReleaseRef();
		if(!alreadyWired)
			vm_page_unreserve_pages(&reservation);
		return -ESRCH;
	}

	cache->temporary = true;
	cache->virtual_end = size;
	cache->Lock();

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)(uintptr_t)*_address;
	addressRestrictions.address_specification = addressSpec;

	status = map_backing_store(locker.AddressSpace(), cache, 0, name, size,
			B_FULL_LOCK, protection, 0, 0, false, &addressRestrictions, true,
			&area, _address, allocationFlags);

	if (status < 0)
		cache->ReleaseRefLocked();

	cache->Unlock();

	if (status == 0) {
		status = arch_vm_set_memory_type(area, physicalAddress,
				protection & (B_MTR_CACHE_MASK | B_DEVICE_AREA));

		if (status != 0)
			delete_area(locker.AddressSpace(), area, false);
	}

	if (status != 0) {
		if(!alreadyWired)
			vm_page_unreserve_pages(&reservation);
		return status;
	}

	VMTranslationMap* map = locker.AddressSpace()->TranslationMap();

	if (alreadyWired) {
		// The area is already mapped, but possibly not with the right
		// memory type.
		map->Lock();
		map->ProtectArea(area, area->protection);
		map->Unlock();
	} else {
		map->Lock();
		map->MapPhysical(area->Base(), physicalAddress, area->Size(),
				area->protection, &reservation);
		map->Unlock();

		vm_page_unreserve_pages(&reservation);
	}

	// modify the pointer returned to be offset back into the new area
	// the same way the physical address in was offset
	*_address += mapOffset;

	return area->id;
}

int vm_map_physical_memory_vecs(pid_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t* _size, uint32_t protection,
	const struct generic_iovec* vecs, uint32_t vecCount)
{
	if (!arch_vm_supports_protection(protection)
		|| (addressSpec & B_MTR_CACHE_MASK) != 0)
	{
		return -EOPNOTSUPP;
	}

	if (vecCount == 0)
		return -EINVAL;

	uintptr_t size = 0;
	for (uint32_t i = 0; i < vecCount; i++) {
		if (vecs[i].base % PAGE_SIZE != 0
			|| vecs[i].length % PAGE_SIZE != 0) {
			return -EINVAL;
		}

		size += vecs[i].length;
	}

	vm_reservation_t reservation;

	size_t reservePages =
			VMAddressSpace::Kernel()->TranslationMap()->MaxPagesNeededToMap(0, size);
	if (!vm_reserve_pages(&reservation, reservePages,
			team == VMAddressSpace::KernelID() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER,
			false,
			false))
	{
		return -ENOMEM;
	}

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked()) {
		vm_page_unreserve_pages(&reservation);
		return -ESRCH;
	}

	// create a device cache
	VMCache* cache;
	int result = VMCacheFactory::CreateDeviceCache(cache, team == VMAddressSpace::KernelID() ? CACHE_DONT_LOCK_KERNEL : 0);
	if (result != 0) {
		vm_page_unreserve_pages(&reservation);
		return result;
	}

	cache->temporary = true;
	cache->virtual_end = size;

	cache->Lock();

	VMArea * area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)*_address;
	addressRestrictions.address_specification = addressSpec & ~B_MTR_CACHE_MASK;
	result = map_backing_store(locker.AddressSpace(), cache, 0, name,
		size, B_FULL_LOCK, protection, 0, 0, false,
		&addressRestrictions, true, &area, _address, team == VMAddressSpace::KernelID() ? CACHE_DONT_LOCK_KERNEL : 0);

	if (result != 0)
		cache->ReleaseRefLocked();

	cache->Unlock();

	if (result != 0) {
		vm_page_unreserve_pages(&reservation);
		return result;
	}

	VMTranslationMap* map = locker.AddressSpace()->TranslationMap();

	map->Lock();

	uint32_t vecIndex = 0;
	size_t vecOffset = 0;

	for (vecIndex = 0; vecIndex < vecCount; ++vecIndex) {
		map->MapPhysical(area->Base() + vecOffset, vecs[vecIndex].base,
				vecs[vecIndex].length, area->protection, &reservation);
		vecOffset += vecs[vecIndex].length;
	}

	map->Unlock();
	vm_page_unreserve_pages(&reservation);

	if (_size != nullptr)
		*_size = size;

	area->cache_type = CACHE_TYPE_DEVICE;
	return area->id;
}

void *kva_map_pages_named(vm_paddr_t start, vm_size_t size, vma_flags_t flags, const char * name)
{
	vaddr_t address = 0;
	uint32_t addressSpec = B_RANDOMIZED_ANY_ADDRESS;
	size_t blockSize = VMAddressSpace::Kernel()->TranslationMap()->HardwareBlockSize();

	if(!(start & (blockSize - 1)) && size >= blockSize)
		addressSpec = B_ANY_KERNEL_BLOCK_ADDRESS;

	int area = vm_map_physical_memory(VMAddressSpace::KernelID(), name, &address,
			addressSpec, size, flags, start, false);
	if (area < 0)
		return nullptr;
	return (void *)address;
}

__warn_references(kva_map_pages_named, "Use of map_physical_memory() is preferred");

void *kva_map_pages(vm_paddr_t start, vm_size_t size, vma_flags_t flags)
{
	char fmap_name[B_OS_NAME_LENGTH];
	snprintf(fmap_name, B_OS_NAME_LENGTH, "physmap %" B_PRIxPHYSADDR, start);
	return kva_map_pages_named(start, size, flags, fmap_name);
}

__warn_references(kva_map_pages, "Use of map_physical_memory() is preferred");

void *kva_map_pfns(vm_pindex_t pfns[], unsigned int nr, vma_flags_t flags)
{
	char area_name[B_OS_NAME_LENGTH];
	vaddr_t addr = 0;

	snprintf(area_name, sizeof(area_name), "pfn map %" PRIx64, (uint64_t)pfns[0]);

	area_id kvaArea = vm_map_physical_memory_pfns(VMAddressSpace::KernelID(),
	                                          "pfnmap",
	                                          &addr,
	                                          B_RANDOMIZED_ANY_ADDRESS,
	                                          nr,
	                                          flags,
	                                          pfns);

	if(kvaArea < 0)
		return nullptr;

	return (void *)addr;
}

__warn_references(kva_map_pfns, "Use of map_physical_memory_pfns() is preferred");

/* Unmap physical pages from VM_MAP range of kernel address space */
void kva_unmap_pages(unsigned long address, unsigned int size)
{
	// check params
	if (size == 0 || (uintptr_t)address + size < (uintptr_t)address
		|| !is_kernelspace_ptr_valid((const void *)address, size))
	{
		return;
	}

	uintptr_t start = rounddown2(address, PAGE_SIZE);
	uintptr_t end = roundup2(address + size, PAGE_SIZE);

	AddressSpaceWriteLocker locker(VMAddressSpace::Kernel(), true);

	do {
	} while (wait_if_address_range_is_wired(locker.AddressSpace(), address,
			size, &locker));

	unmap_address_range(locker.AddressSpace(), start, end - start, true);
}

__warn_references(kva_unmap_pages, "Use of delete_area() is preferred");

vma_flags_t vm_map_user_prot(int prot)
{
	vma_flags_t flags = VMA_FLAG_S_READ;

	if (prot & PROT_READ) {
		flags |= VMA_FLAG_U_READ;
	}

	if(prot & PROT_WRITE) {
		flags |= VMA_FLAG_U_WRITE | VMA_FLAG_S_WRITE;
	}

	if(prot & PROT_EXECUTE) {
		flags &= ~(VMA_FLAG_U_WRITE | VMA_FLAG_S_WRITE);
		flags |= VMA_FLAG_U_EXECUTE;
	}

	if(prot & PROT_NONSECURE) {
		flags &= ~(VMA_FLAG_U_EXECUTE | VMA_FLAG_S_EXECUTE);
		flags |= VMA_FLAG_NONSECURE;
	}

	return flags;
}

unsigned int vm_num_page_faults(void)
{
	return __atomic_load_n(&sNumPageFaults, __ATOMIC_RELAXED);
}

int vm_area_for(uintptr_t address, bool kernel)
{
	uint32_t team;
	if (is_userspace_ptr_valid((const void *)address, 1)) {
		// we try the user team address space, if any
		team = VMAddressSpace::CurrentID();
	} else {
		team = VMAddressSpace::KernelID();
	}

	AddressSpaceReadLocker locker(team);
	if (!locker.IsLocked())
		return -ESRCH;

	VMArea* area = locker.AddressSpace()->LookupArea(address);
	if (area != nullptr) {
		if (!kernel && (area->protection & (B_READ_AREA | B_WRITE_AREA)) == 0)
			return -EINVAL;

		return area->id;
	}

	return -EINVAL;
}

int __sys_area_for(void* address)
{
	return vm_area_for((uintptr_t)address, false);
}

static void fill_area_info(struct VMArea* area, area_info* info)
{
	strlcpy(info->name, area->name, sizeof(info->name));
	info->area = area->id;
	info->address = (void*)area->Base();
	info->size = area->Size();
	info->protection = area->protection;
	if(area->cache_type >= CACHE_TYPE_DEVICE)
		info->protection |= B_DEVICE_AREA;
	info->lock = B_FULL_LOCK;
	info->team = area->address_space->ID();
	info->copy_count = 0;
	info->in_count = 0;
	info->out_count = 0;
	info->ram_size = 0;

	if(area->cache) {
		VMCache* cache = vm_area_get_locked_cache(area);
		// Note, this is a simplification; the cache could be larger than this area
		info->ram_size = cache->page_count * PAGE_SIZE;
		vm_area_put_locked_cache(cache);
	}
}

int _get_area_info(area_id id, area_info* info, size_t)
{
	if (info == nullptr)
		return -EINVAL;

	AddressSpaceReadLocker locker;
	VMArea * area;
	int status = locker.SetFromArea(id, area);
	if (status != 0)
		return status;

	fill_area_info(area, info);
	return 0;
}

static int _get_next_area_info_impl(pid_t team, ssize_t *cookie,
		area_info *info, size_t, uintptr_t terminator_cookie) {
	uintptr_t nextBase = *(uintptr_t*) cookie;

	// we're already through the list
	if (nextBase == terminator_cookie)
		return -ENOENT;

	if (team == 0)
		team = getpid();

	AddressSpaceReadLocker locker(team);
	if (!locker.IsLocked())
		return -ESRCH;

	VMArea *area = locker.AddressSpace()->FindClosestArea(nextBase, false);
	if (area == nullptr) {
		*cookie = terminator_cookie;
		return -ENOENT;
	}

	fill_area_info(area, info);
	*cookie = (ssize_t) (area->Base() + 1);

	return 0;
}

int _get_next_area_info(pid_t team, ssize_t *cookie, area_info *info,
		size_t size) {
	return _get_next_area_info_impl(team, cookie, info, size, ~0UL);
}

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
struct area_info32 {
	area_id		area;
	char		name[B_OS_NAME_LENGTH];
	uint32_t	size;
	uint32_t	lock;
	uint32_t	protection;
	uint32_t	team;
	uint32_t	ram_size;
	uint32_t	copy_count;
	uint32_t	in_count;
	uint32_t	out_count;
	uint32_t	address;
};
#endif

static size_t size_of_area_info()
{
#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	if(current_team_is_32bit()) {
		return sizeof(area_info32);
	}
#endif

	return sizeof(area_info);
}

static int area_info_to_user(void * userPointer, const area_info& info, size_t size)
{
#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	if(current_team_is_32bit()) {
		area_info32 info32;
		info32.area = info.area;
		memcpy(info32.name, info.name, B_OS_NAME_LENGTH);
		info32.size = info.size;
		info32.lock = info.lock;
		info32.protection = info.protection;
		info32.team = info.team;
		info32.ram_size = info.ram_size;
		info32.copy_count = info.copy_count;
		info32.in_count = info.in_count;
		info32.out_count = info.out_count;
		info32.address = (uintptr_t)info.address;

		return memcpy_to_user(userPointer, &info32, size);
	}
#endif

	return memcpy_to_user(userPointer, &info, size);
}

int __sys_get_area_info(int area, area_info* userInfo, size_t size)
{
	if (!is_userspace_ptr_valid(userInfo, size))
		return -EFAULT;

	if (size > size_of_area_info())
		size = size_of_area_info();

	area_info info{};
	int status = _get_area_info(area, &info, sizeof(area_info));
	if (status != 0)
		return status;

	if (area_info_to_user(userInfo, info, size) < 0)
		return -EFAULT;

	return 0;
}

int __sys_get_next_area_info(uint32_t team, ssize_t *userCookie,
		area_info *userInfo, size_t size)
{
	ssize_t cookie = 0;
	uintptr_t terminatorCookie;
#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	bool prohibitEnumaration = false;
#endif

	if (team == B_CURRENT_TEAM)
		team = getpid();

	if (!is_userspace_ptr_valid(userInfo, size))
		return -EFAULT;

	if (size > size_of_area_info())
		size = size_of_area_info();

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	if(current_team_is_32bit()) {
		if (memcpy_from_user(&cookie, userCookie, sizeof(uint32_t)) < 0)
			return -EFAULT;
		terminatorCookie = 0xffffffff;
		if(team == B_SYSTEM_TEAM) {
			// 32bit processes can't enumerate kernel addresses
			// as pointer would overflow
			prohibitEnumaration = true;
		}
	} else
#endif
	{
		if (memcpy_from_user(&cookie, userCookie, sizeof(ssize_t)) < 0)
			return -EFAULT;
		terminatorCookie = ~0UL;
	}

	area_info info;
	int status;

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	if (!prohibitEnumaration) {
#endif
		status = _get_next_area_info_impl(team, &cookie, &info,
				sizeof(area_info), terminatorCookie);
#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	} else {
		cookie = terminatorCookie;
		status = -ENOENT;
	}
#endif

	if (status != 0)
		return status;

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
	if(current_team_is_32bit()) {
		if (memcpy_to_user(userCookie, &cookie, sizeof(uint32_t)))
			return -EFAULT;
	} else
#endif
	{
		if (memcpy_to_user(userCookie, &cookie, sizeof(ssize_t)))
			return -EFAULT;
	}

	if (area_info_to_user(userInfo, info, size) < 0)
		return -EFAULT;

	return 0;
}

int vm_delete_area(uint32_t team, int id, bool kernel)
{
	// lock the address space and make sure the area isn't wired
	AddressSpaceWriteLocker locker;
	VMArea* area;
	AreaCacheLocker cacheLocker;

	do {
		int status = locker.SetFromArea(team, id, area);
		if (status != 0)
			return status;
		cacheLocker.SetTo(area);
	} while (wait_if_area_is_wired(area, &locker, &cacheLocker));

	cacheLocker.Unlock();

	// SetFromArea will have returned an error if the area's owning team is not
	// the same as the passed team, so we don't need to do those checks here.

	if (!kernel && (area->protection & B_KERNEL_AREA) != 0)
		return -EPERM;

	delete_area(locker.AddressSpace(), area, false);
	return 0;
}

int delete_area(int area)
{
	return vm_delete_area(VMAddressSpace::KernelID(), area, true);
}

int __sys_delete_area(int area)
{
	return vm_delete_area(VMAddressSpace::CurrentID(), area, false);
}

int __sys_munmap(uintptr_t start, size_t size)
{
	return uvm_unmap_user_memory(VMAddressSpace::CurrentID(), start, size);
}

int vm_create_null_area(uint32_t team, const char* name, vaddr_t * address,
	uint32_t addressSpec, size_t size, uint32_t flags)
{
	uint32_t allocationFlags = 0;

	if(flags & CREATE_AREA_DONT_WAIT)
		allocationFlags |= CACHE_DONT_WAIT_FOR_MEMORY;

	size = roundup2(size, PAGE_SIZE);

	// Lock the address space and, if B_EXACT_ADDRESS and
	// CREATE_AREA_UNMAP_ADDRESS_RANGE were specified, ensure the address range
	// is not wired.
	AddressSpaceWriteLocker locker;
	do {
		if (locker.SetTo(team) != 0)
			return -ESRCH;
	} while (addressSpec == B_EXACT_ADDRESS
		&& (flags & CREATE_AREA_UNMAP_ADDRESS_RANGE) != 0
		&& wait_if_address_range_is_wired(locker.AddressSpace(),
			(uintptr_t)*address, size, &locker));

	// create a null cache
	int priority = (flags & CREATE_AREA_PRIORITY_VIP) != 0
		? GFP_PRIO_VIP : GFP_PRIO_SYSTEM;

	if(locker.AddressSpace() == VMAddressSpace::Kernel()) {
		allocationFlags = CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY;
	}

	VMCache* cache;
	int status = VMCacheFactory::CreateNullCache(priority, cache, allocationFlags);
	if (status != 0)
		return status;

	cache->temporary = true;
	cache->virtual_end = size;

	cache->Lock();

	VMArea* area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)*address;
	addressRestrictions.address_specification = addressSpec;

	status = map_backing_store(locker.AddressSpace(), cache, 0, name, size,
			B_LAZY_LOCK, 0, 0, flags, false, &addressRestrictions, true,
			&area, address, allocationFlags);

	if (status < 0) {
		cache->ReleaseRefAndUnlock();
		return status;
	}

	area->cache_type = CACHE_TYPE_NULL;

	cache->Unlock();

	return area->id;
}

int vm_map_physical_memory_pfns(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t countPfns, vma_flags_t protection,
	const vm_pindex_t * pfnArray)
{
	VMArea* area;
	VMCache* cache;
	uint32_t allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY;
	scoped_vm_reservation_t pageReservation;

	if (!countPfns)
		return -EINVAL;

	if (!arch_vm_supports_protection(protection))
		return -EINVAL;

	fix_protection(&protection);

	{
		size_t reservePages = VMAddressSpace::Kernel()->TranslationMap()->MaxPagesNeededToMap(0, countPfns << PAGE_SHIFT);

		if (!vm_reserve_pages(&pageReservation, reservePages,
				team == (uint32_t)VMAddressSpace::KernelID() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER,
				false, false))
		{
			return -ENOMEM;
		}
	}

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return -ESRCH;

	// create a device cache
	int status = VMCacheFactory::CreateDeviceCache(cache, allocationFlags);
	if (status != 0)
		return status;

	cache->temporary = true;
	cache->virtual_end = countPfns << PAGE_SHIFT;

	cache->Lock();

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)*_address;
	addressRestrictions.address_specification = addressSpec;

	if(locker.AddressSpace() == VMAddressSpace::Kernel()) {
		allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
	} else {
		allocationFlags = 0;
	}

	status = map_backing_store(locker.AddressSpace(), cache, 0, name,
			countPfns << PAGE_SHIFT,
			B_FULL_LOCK, protection, 0, 0, false,
			&addressRestrictions, true, &area, _address, allocationFlags);

	if (status < 0)
		cache->ReleaseRefLocked();

	cache->Unlock();

	if (status != 0)
		return status;

	area->cache_type = (protection & VMA_FLAG_NONSECURE) ? CACHE_TYPE_NONSECURE_PFNMAP :
			CACHE_TYPE_PFNMAP;

	VMTranslationMap * map = locker.AddressSpace()->TranslationMap();

	map->Lock();

	for(size_t i = 0 ; i < countPfns ; ++i) {
		map->Map(area->Base() + (i * PAGE_SIZE),
				((vm_paddr_t)pfnArray[i]) << PAGE_SHIFT,
				area->protection,
				&pageReservation);
	}

	map->Unlock();

	return area->id;
}

int vm_create_anonymous_area_etc(pid_t team, const char *name, size_t size,
	uint32_t wiring, uint32_t protection, uint32_t flags, size_t guardSize,
	const virtual_address_restrictions* virtualAddressRestrictions,
	const physical_address_restrictions* physicalAddressRestrictions,
	bool kernel, vaddr_t* _address)
{
	VMArea * area;
	VMCache* cache;
	struct page * contigPages = nullptr;
	bool isStack = (protection & (B_STACK_AREA | B_KERNEL_STACK_AREA)) != 0;
	vm_pindex_t guardPages;
	bool canOvercommit = false;
	uint32_t pageAllocFlags =
			(flags & CREATE_AREA_DONT_CLEAR) == 0 ? VM_PAGE_ALLOC_CLEAR : 0;
	int mallocFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
	struct page * page = nullptr;

	size = ROUND_PGUP(size);
	guardSize = ROUND_PGUP(guardSize);
	guardPages = guardSize / PAGE_SIZE;

	if (size == 0 || size < guardSize)
		return -EINVAL;
	if (!arch_vm_supports_protection(protection))
		return -EINVAL;

	// These can't be created with such type
	if (protection & (B_NONSECURE_AREA | B_DEVICE_AREA))
		return -EINVAL;

	if (isStack  || (protection & B_OVERCOMMITTING_AREA) != 0)
		canOvercommit = true;

	// check parameters
	switch (virtualAddressRestrictions->address_specification) {
		case B_ANY_ADDRESS:
		case B_EXACT_ADDRESS:
		case B_BASE_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
		case B_RANDOMIZED_ANY_ADDRESS:
		case B_RANDOMIZED_BASE_ADDRESS:
			break;

		default:
			return -EINVAL;
	}

	// If low or high physical address restrictions are given, we force
	// B_CONTIGUOUS wiring, since only then we'll use
	// vm_page_allocate_page_run() which deals with those restrictions.
	if (physicalAddressRestrictions->low_address != 0
		|| physicalAddressRestrictions->high_address != 0) {
		wiring = B_CONTIGUOUS;
	}

	physical_address_restrictions stackPhysicalRestrictions;
	bool doReserveMemory = false;
	switch (wiring) {
		case B_NO_LOCK:
			break;
		case B_FULL_LOCK:
		case B_LAZY_LOCK:
		case B_CONTIGUOUS:
			doReserveMemory = true;
			break;
		case B_ALREADY_WIRED:
			break;
		case B_LOMEM:
			stackPhysicalRestrictions = *physicalAddressRestrictions;
#if defined(__arm__) || defined(__aarch64__)
			stackPhysicalRestrictions.high_address = 0xFFFFF000;
#else
			stackPhysicalRestrictions.high_address = 16 * 1024 * 1024;
#endif
			physicalAddressRestrictions = &stackPhysicalRestrictions;
			wiring = B_CONTIGUOUS;
			doReserveMemory = true;
			break;
		case B_32_BIT_FULL_LOCK:
			wiring = B_FULL_LOCK;
			doReserveMemory = true;
			break;
		case B_32_BIT_CONTIGUOUS:
			wiring = B_CONTIGUOUS;
			doReserveMemory = true;
			break;
		default:
			return -EINVAL;
	}

	// Optimization: For a single-page contiguous allocation without low/high
	// memory restriction B_FULL_LOCK wiring suffices.
	if (wiring == B_CONTIGUOUS && size == PAGE_SIZE
		&& physicalAddressRestrictions->low_address == 0
		&& physicalAddressRestrictions->high_address == 0) {
		wiring = B_FULL_LOCK;
	}

	// For full lock or contiguous areas we're also going to map the pages and
	// thus need to reserve pages for the mapping backend upfront.
	size_t reservedMapPages = 0;
	if (wiring == B_FULL_LOCK || wiring == B_CONTIGUOUS) {
		AddressSpaceReadLocker locker;
		int status = locker.SetTo(team);
		if (status != 0)
			return status;

		VMTranslationMap *map = locker.AddressSpace()->TranslationMap();
		reservedMapPages = map->MaxPagesNeededToMap(0, size);
	}

	int priority;
	if (team != VMAddressSpace::KernelID()) {
		priority = GFP_PRIO_USER;
	} else if ((flags & CREATE_AREA_PRIORITY_VIP) != 0) {
		priority = GFP_PRIO_VIP;
		mallocFlags |= CACHE_USE_RESERVE;
	} else {
		priority = GFP_PRIO_SYSTEM;
	}

	// Reserve memory before acquiring the address space lock. This reduces the
	// chances of failure, since while holding the write lock to the address
	// space (if it is the kernel address space that is), the low memory handler
	// won't be able to free anything for us.
	size_t reservedMemory = 0;
	if (doReserveMemory) {
		time_t timeout = (flags & CREATE_AREA_DONT_WAIT) != 0 ? 0 : SECONDS(1);
		if (vm_try_reserve_memory(size, priority, timeout) != 0)
			return -ENOMEM;
		reservedMemory = size;
		// TODO: We don't reserve the memory for the pages for the page
		// directories/tables. We actually need to do since we currently don't
		// reclaim them (and probably can't reclaim all of them anyway). Thus
		// there are actually less physical pages than there should be, which
		// can get the VM into trouble in low memory situations.
	}

	AddressSpaceWriteLocker locker;
	VMAddressSpace * addressSpace;
	int status;

	// For full lock areas reserve the pages before locking the address
	// space. E.g. block caches can't release their memory while we hold the
	// address space lock.
	size_t reservedPages = reservedMapPages;
	if (wiring == B_FULL_LOCK || wiring == B_CONTIGUOUS)
		reservedPages += size / PAGE_SIZE;

	vm_reservation_t reservation;
	if (reservedPages > 0) {
		if (!vm_reserve_pages(&reservation,
				reservedPages,
				priority,
				(flags & CREATE_AREA_DONT_WAIT) != 0,
				false))
		{
			reservedPages = 0;
			status = -ENOMEM;
			goto err;
		}
	}

	if (wiring == B_CONTIGUOUS) {
		contigPages = vm_page_allocate_page_run(
				PAGE_STATE_WIRED | pageAllocFlags, size / PAGE_SIZE,
				physicalAddressRestrictions, priority, &reservation);

		if (contigPages == nullptr) {
			status = -ENOMEM;
			goto err;
		}
	}

	// Lock the address space and, if B_EXACT_ADDRESS and
	// CREATE_AREA_UNMAP_ADDRESS_RANGE were specified, ensure the address range
	// is not wired.
	do {
		status = locker.SetTo(team);
		if (status != 0)
			goto err;

		addressSpace = locker.AddressSpace();
	} while (virtualAddressRestrictions->address_specification
			== B_EXACT_ADDRESS
		&& (flags & CREATE_AREA_UNMAP_ADDRESS_RANGE) != 0
		&& wait_if_address_range_is_wired(addressSpace,
			(uintptr_t)virtualAddressRestrictions->address, size, &locker));

	// create an anonymous cache
	// if it's a stack, make sure that two pages are available at least
	status = VMCacheFactory::CreateAnonymousCache(cache, canOvercommit,
		isStack ? (MIN(2, size / PAGE_SIZE - guardPages)) : 0, guardPages,
		wiring == B_NO_LOCK, priority, mallocFlags);
	if (status != 0)
		goto err;

	cache->temporary = true;
	cache->virtual_end = size;
	cache->committed_size = reservedMemory;
		// TODO: This should be done via a method.
	reservedMemory = 0;

	cache->Lock();

	status = map_backing_store(addressSpace, cache, 0, name, size, wiring,
		protection, 0, flags, false, virtualAddressRestrictions,
		kernel, &area, _address, mallocFlags);

	if (status != 0) {
		cache->ReleaseRefAndUnlock();
		goto err;
	}

	locker.DegradeToReadLock();

	switch (wiring) {
		case B_NO_LOCK:
		case B_LAZY_LOCK:
			// do nothing - the pages are mapped in as needed
			break;

		case B_FULL_LOCK:
		{
			// Allocate and map all pages for this area

			off_t offset = 0;
			for (uintptr_t address = area->Base();
					address < area->Base() + (area->Size() - 1);
					address += PAGE_SIZE, offset += PAGE_SIZE)
			{
				if(address < (area->Base() + guardSize))
					continue;

				page = vm_page_allocate_page_etc(&reservation,
					PAGE_STATE_WIRED | pageAllocFlags);

				// Here we get initial page reference
				cache->InsertPage(page, offset);

				// map_page for full lock can't return any error
				// as everything is preallocated
				map_page(area, page, address, protection, &reservation);
			}

			break;
		}

		case B_ALREADY_WIRED:
		{
			// The pages should already be mapped. This is only really useful
			// during boot time. Find the appropriate vm_page objects and stick
			// them in the cache object.
			VMTranslationMap * map = addressSpace->TranslationMap();
			off_t offset = 0;

			if (!gKernelStartup)
				panic("B_ALREADY_WIRED flag used outside kernel startup\n");

			map->Lock();

			for (uintptr_t virtualAddress = area->Base();
					virtualAddress < area->Base() + (area->Size() - 1);
					virtualAddress += PAGE_SIZE, offset += PAGE_SIZE) {
				vm_paddr_t physicalAddress;
				uint32_t queryFlags;

				status = map->Query(virtualAddress, &physicalAddress, &queryFlags);

				if (status < 0) {
					panic("looking up mapping failed for va %#" PRIxPTR "\n",
						virtualAddress);
				}

				if(physicalAddress == 0) {
					// Skip - this is used e.g. by kernel stacks
					continue;
				}

				page = phys_to_page(physicalAddress);

				if (page == nullptr) {
					panic("looking up page failed for pa %#" B_PRIxPHYSADDR
						"\n", physicalAddress);
				}

				// Here we get initial reference counter
				cache->InsertPage(page, offset);
				increment_page_wired_count(page);
				vm_page_set_state(page, PAGE_STATE_WIRED);
				page->busy = 0;
			}

			map->Unlock();
			break;
		}

		case B_CONTIGUOUS:
		{
			// We have already allocated our continuous pages run, so we can now
			// just map them in the address space
			VMTranslationMap* map = addressSpace->TranslationMap();
			vm_paddr_t physicalAddress = page_to_phys(contigPages);
			uintptr_t virtualAddress;
			off_t offset = 0;

			map->Lock();

			for (virtualAddress = area->Base(); virtualAddress < area->Base()
					+ (area->Size() - 1); virtualAddress += PAGE_SIZE,
					offset += PAGE_SIZE, physicalAddress += PAGE_SIZE) {
				page = phys_to_page(physicalAddress);
				if (page == nullptr)
					panic("couldn't lookup physical page just allocated\n");
				map->Map(virtualAddress, physicalAddress, protection, &reservation);
				cache->InsertPage(page, offset);
				increment_page_wired_count(page);
			}

			map->Unlock();
			break;
		}

		default:
			break;
	}

	cache->Unlock();

	if (reservedPages > 0)
		vm_page_unreserve_pages(&reservation);

	area->cache_type = CACHE_TYPE_RAM;
	return area->id;

err:
	if (reservedPages > 0)
		vm_page_unreserve_pages(&reservation);
	if (reservedMemory > 0)
		vm_unreserve_memory(reservedMemory);

	return status;
}

struct PageFaultContext {
	AddressSpaceReadLocker	addressSpaceLocker;
	VMCacheChainLocker		cacheChainLocker;

	VMTranslationMap *		map;
	VMCache*				topCache = nullptr;
	off_t					cacheOffset = 0;
	vm_reservation_t		reservation;
	size_t					reserve_count = 0;
#if defined(CONFIG_MEMORY_HOTPLUG)
	vm_reservation_t		system_reservation;
	size_t					system_reserve_count = 0;
#endif
	bool					isWrite;

	// return values
	struct page*			page = nullptr;
	bool					restart = false;
	bool					pageAllocated = false;


	PageFaultContext(VMAddressSpace * addressSpace, bool isWrite_)
		:
		addressSpaceLocker(addressSpace, true),
		map(addressSpace->TranslationMap()),
		isWrite(isWrite_)
	{
	}

	~PageFaultContext()
	{
		UnlockAll();
		if(reserve_count > 0)
			vm_page_unreserve_pages(&reservation);
#if defined(CONFIG_MEMORY_HOTPLUG)
		if(system_reserve_count > 0)
			vm_page_unreserve_pages(&system_reservation);
#endif
	}

	void Prepare(VMCache* top_cache, off_t cache_offset)
	{
		ASSERT(top_cache);

		this->topCache = top_cache;
		this->cacheOffset = cache_offset;
		page = nullptr;
		restart = false;
		pageAllocated = false;

		cacheChainLocker.SetTo(top_cache);
	}

	void UnlockAll(VMCache* exceptCache = nullptr)
	{
		topCache = nullptr;
		addressSpaceLocker.Unlock();
		cacheChainLocker.Unlock(exceptCache);
	}
};


/*!	Gets the page that should be mapped into the area.
	Returns an error code other than \c 0, if the page couldn't be found or
	paged in. The locking state of the address space and the caches is undefined
	in that case.
	Returns \c 0 with \c context.restart set to \c true, if the functions
	had to unlock the address space and all caches and is supposed to be called
	again.
	Returns \c 0 with \c context.restart set to \c false, if the page was
	found. It is returned in \c context.page. The address space will still be
	locked as well as all caches starting from the top cache to at least the
	cache the page lives in.
*/
static int fault_get_page(PageFaultContext& context)
{
	VMCache* cache = context.topCache;
	VMCache* lastCache = nullptr;
	struct page * page = nullptr;

	if (!cache)
		return -EINVAL;

	while (cache != nullptr) {
		// We already hold the lock of the cache at this point.

		lastCache = cache;

		page = cache->LookupPage(context.cacheOffset);
		if (page != nullptr && page->busy) {
			// page must be busy -- wait for it to become unbusy
			context.UnlockAll(cache);
			cache->ReleaseRefLocked();
			cache->WaitForPageEvents(page, PAGE_EVENT_NOT_BUSY, false);

			// restart the whole process
			context.restart = true;
			return 0;
		}

		if (page != nullptr)
			break;

		// The current cache does not contain the page we're looking for.
		// see if the backing store has it
		if (cache->HasPage(context.cacheOffset)) {
			// insert a fresh page and mark it busy -- we're going to read it in
			page = vm_page_allocate_page_etc(&context.reservation, PAGE_STATE_ACTIVE | VM_PAGE_ALLOC_BUSY);
			cache->InsertPage(page, context.cacheOffset);

			// We need to unlock all caches and the address space while reading
			// the page in. Keep a reference to the cache around.
			cache->AcquireRefLocked();
			context.UnlockAll();

			// read the page in
			generic_iovec vec;
			vec.base = page_to_phys(page);
			vec.length = PAGE_SIZE;

			size_t bytesRead = PAGE_SIZE;

			int status = cache->Read(context.cacheOffset, &vec, GENERIC_IO_USE_PHYSICAL, 1, &bytesRead);

			cache->Lock();

			if (status < 0) {
				// on error remove and free the page
				cache->NotifyPageEvents(page, PAGE_EVENT_NOT_BUSY);
				cache->RemovePage(page);
				vm_page_set_state(page, PAGE_STATE_FREE);
				cache->ReleaseRefAndUnlock();
				return status;
			}

			// mark the page unbusy again
			cache->MarkPageUnbusy(page);

			// Since we needed to unlock everything temporarily, the area
			// situation might have changed. So we need to restart the whole
			// process.
			cache->ReleaseRefAndUnlock();
			context.restart = true;
			return 0;
		}

		cache = context.cacheChainLocker.LockSourceCache();
	}

	if (page == nullptr) {
		// There was no adequate page, determine the cache for a clean one.
		// Read-only pages come in the deepest cache, only the top most cache
		// may have direct write access.
		cache = context.isWrite ? context.topCache : lastCache;

		// allocate a clean page
		page = vm_page_allocate_page_etc(&context.reservation, PAGE_STATE_ACTIVE | VM_PAGE_ALLOC_CLEAR);

		// insert the new page into our cache
		cache->InsertPage(page, context.cacheOffset);
		context.pageAllocated = true;
	} else if (page->Cache() != context.topCache && context.isWrite) {
		// We have a page that has the data we want, but in the wrong cache
		// object so we need to copy it and stick it into the top cache.
		struct page * sourcePage = page;

		// TODO: If memory is low, it might be a good idea to steal the page
		// from our source cache -- if possible, that is.
		page = vm_page_allocate_page_etc(&context.reservation, PAGE_STATE_ACTIVE);

		// To not needlessly kill concurrency we unlock all caches but the top
		// one while copying the page. Lacking another mechanism to ensure that
		// the source page doesn't disappear, we mark it busy.
		sourcePage->busy = true;
		context.cacheChainLocker.UnlockKeepRefs(true);

		vm_memcpy_physical_page(page_to_phys(page), page_to_phys(sourcePage));

		context.cacheChainLocker.RelockCaches(true);
		sourcePage->Cache()->MarkPageUnbusy(sourcePage);

		// insert the new page into our cache
		context.topCache->InsertPage(page, context.cacheOffset);
		context.pageAllocated = true;
	}
	context.page = page;
	return 0;
}


/*!	Makes sure the address in the given address space is mapped.

	\param addressSpace The address space.
	\param originalAddress The address. Doesn't need to be page aligned.
	\param isWrite If \c true the address shall be write-accessible.
	\param isUser If \c true the access is requested by a userland team.
	\param wirePage On success, if non \c NULL, the wired count of the page
		mapped at the given address is incremented and the page is returned
		via this parameter.
	\return \c 0 on success, another error code otherwise.
*/
static int vm_soft_fault(VMAddressSpace * addressSpace, uintptr_t originalAddress,
	bool isWrite, bool isExecute, bool isUser, struct page ** wirePage)
{
	PageFaultContext context(addressSpace, isWrite);

	uintptr_t address = ROUND_PGDOWN(originalAddress);
	int status = 0;
	int priority;

#if defined(CONFIG_MEMORY_HOTPLUG)
	int sysPriority;
#endif

	__atomic_fetch_add(&sNumPageFaults, 1, __ATOMIC_RELAXED);
	addressSpace->IncrementFaultCount();

	// We may need up to 2 pages plus pages needed for mapping them -- reserving
	// the pages upfront makes sure we don't have any cache locked, so that the
	// page daemon/thief can do their job without problems.
	size_t reservePages = 2;
	size_t sysReservePages = context.map->MaxPagesNeededToMap(0, PAGE_SIZE);

	if(addressSpace == VMAddressSpace::Kernel()) {
		priority = GFP_PRIO_SYSTEM;
#if defined(CONFIG_MEMORY_HOTPLUG)
		sysPriority = GFP_PRIO_SYSTEM;
#endif
	} else {
#if defined(CONFIG_MEMORY_HOTPLUG)
		if(addressSpace->IsExternalMemoryAllowed()) {
			priority = GFP_PRIO_EXTERNAL;
		} else
#endif
		{
			priority = GFP_PRIO_USER;
		}
#if defined(CONFIG_MEMORY_HOTPLUG)
		sysPriority = GFP_PRIO_USER;
#endif
	}

	context.addressSpaceLocker.Unlock();

	/*
	 * This function will not trigger OOM killer.
	 * In case vm_soft_fault() returns -ENOMEM,
	 * the trap handler will dispatch OOM killer
	 * as needed (only for user space tasks).
	 */

	if(!vm_reserve_pages(&context.reservation,
#if defined(CONFIG_MEMORY_HOTPLUG)
			reservePages,
#else
			reservePages + sysReservePages,
#endif
			priority,
			false,
			false))
	{
		return -ENOMEM;
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	context.reserve_count = reservePages;
#else
	context.reserve_count = reservePages + sysReservePages;
#endif

#if defined(CONFIG_MEMORY_HOTPLUG)
	if(!vm_reserve_pages(&context.system_reservation,
			sysReservePages,
			sysPriority,
			false,
			false))
	{
		return -ENOMEM;
	}

	context.system_reserve_count = sysReservePages;
#endif

	while (true) {
		context.addressSpaceLocker.Lock();

		// get the area the fault was in
		VMArea * area = addressSpace->LookupArea(address);

		if (area == nullptr) {
			status = -EFAULT;
			break;
		}

		if (area->protection & B_NONSECURE_AREA) {
			printk(KERN_INFO, "Fault in Non-Secure area at %p\n",
					(void* )address);
			status = -EFAULT;
			break;
		}

		if (area->protection & B_DEVICE_AREA) {
			printk(KERN_INFO, "Fault in device area at %p\n", (void* )address);
			status = -EFAULT;
			break;
		}

		// check permissions
		uint32_t protection = get_area_page_protection(area, address);
		if (isUser && (protection & B_USER_PROTECTION) == 0) {
			status = -EPERM;
			break;
		}

		if (isWrite && (protection
				& (B_WRITE_AREA | (isUser ? 0 : B_KERNEL_WRITE_AREA))) == 0)
		{
			printk(KERN_ERR, "Write to protected address %p\n", (void *)address);
			status = -EPERM;
			break;
		} else if (isExecute && (protection
				& (isUser ? B_EXECUTE_AREA : B_KERNEL_EXECUTE_AREA)) == 0) {
			printk(KERN_ERR, "Execute from non-executable address %p\n", (void *)address);
			status = -EPERM;
			break;
		} else if (!isWrite && !isExecute && (protection
				& (B_READ_AREA | (isUser ? 0 : B_KERNEL_WRITE_AREA))) == 0) {
			printk(KERN_ERR, "Trying to access kernel address %p\n", (void *)address);
			status = -EPERM;
			break;
		}

		if(!area->cache) {
			printk(KERN_ERR, "Faulting in cache-less are not allowed\n");
			status = -EPERM;
			break;
		}

#if defined(CONFIG_MEMORY_HOTPLUG)
		if(area->wiring != B_NO_LOCK) {
			// If area uses locked wiring, we can't allow memory allocation
			// from external zone. Downgrade memory reservation if needed
			// and restart the check
			if(priority == GFP_PRIO_EXTERNAL) {
				context.addressSpaceLocker.Unlock();
				vm_page_unreserve_pages(&context.reservation);
				context.reserve_count = 0;
				priority = GFP_PRIO_USER;

				if(!vm_reserve_pages(&context.reservation,
						reservePages,
						priority,
						false,
						false))
				{
					return -ENOMEM;
				}
				context.reserve_count = reservePages;
				continue;
			}
		}
#endif

		// We have the area, it was a valid access, so let's try to resolve the
		// page fault now.
		// At first, the top most cache from the area is investigated.

		context.Prepare(vm_area_get_locked_cache(area),
			address - area->Base() + area->cache_offset);

		// See if this cache has a fault handler -- this will do all the work
		// for us.
		{
			// Note, since the page fault is resolved with interrupts enabled,
			// the fault handler could be called more than once for the same
			// reason -- the store must take this into account.
			status = context.topCache->Fault(addressSpace, context.cacheOffset);

			if (status != VM_FAULT_DONT_HANDLE)
				break;
		}

		// The top most cache has no fault handler, so let's see if the cache or
		// its sources already have the page we're searching for (we're going
		// from top to bottom).
		status = fault_get_page(context);
		if (status != 0) {
			break;
		}

		if (context.restart)
			continue;

		// All went fine, all there is left to do is to map the page into the
		// address space.

		// If the page doesn't reside in the area's cache, we need to make sure
		// it's mapped in read-only, so that we cannot overwrite someone else's
		// data (copy-on-write)
		uint32_t newProtection = protection;
		if (context.page->Cache() != context.topCache && !isWrite)
			newProtection &= ~(B_WRITE_AREA | B_KERNEL_WRITE_AREA);

		bool unmapPage = false;
		bool mapPage = true;

		// check whether there's already a page mapped at the address
		context.map->Lock();

		vm_paddr_t physicalAddress;
		uint32_t flags;
		struct page * mappedPage = nullptr;

		if(context.map->Query(address, &physicalAddress, &flags) == 0
			&& (flags & PAGE_PRESENT) != 0
			&& (mappedPage = phys_to_page(physicalAddress)) != nullptr)
		{
			// Yep there's already a page. If it's ours, we can simply adjust
			// its protection. Otherwise we have to unmap it.
			if (mappedPage == context.page) {
				context.map->ProtectPage(area, address, newProtection);
					// Note: We assume that ProtectPage() is atomic (i.e.
					// the page isn't temporarily unmapped), otherwise we'd have
					// to make sure it isn't wired.
				mapPage = false;
			} else
				unmapPage = true;
		}

		context.map->Unlock();

		if (unmapPage) {
			// If the page is wired, we can't unmap it. Wait until it is unwired
			// again and restart. Note that the page cannot be wired for
			// writing, since it it isn't in the topmost cache. So we can safely
			// ignore ranges wired for writing (our own and other concurrent
			// wiring attempts in progress) and in fact have to do that to avoid
			// a deadlock.
			VMAreaUnwiredWaiter waiter;
			if (area->AddWaiterIfWired(&waiter, address, PAGE_SIZE,
					VMArea::IGNORE_WRITE_WIRED_RANGES))
			{
				// unlock everything and wait
				if (context.pageAllocated) {
					ASSERT(context.topCache);

					// ... but since we allocated a page and inserted it into
					// the top cache, remove and free it first. Otherwise we'd
					// have a page from a lower cache mapped while an upper
					// cache has a page that would shadow it.
					context.topCache->RemovePage(context.page);
#if defined(CONFIG_MEMORY_HOTPLUG)
					vm_page_free_etc(context.topCache, context.page, &context.system_reservation);
#else
					vm_page_free_etc(context.topCache, context.page, &context.reservation);
#endif
					context.pageAllocated = false;
					context.page = nullptr;
				}

				context.UnlockAll();
				waiter.waitEntry.Wait();
				continue;
			}

			// Note: The mapped page is a page of a lower cache. We are
			// guaranteed to have that cached locked, our new page is a copy of
			// that page, and the page is not busy. The logic for that guarantee
			// is as follows: Since the page is mapped, it must live in the top
			// cache (ruled out above) or any of its lower caches, and there is
			// (was before the new page was inserted) no other page in any
			// cache between the top cache and the page's cache (otherwise that
			// would be mapped instead). That in turn means that our algorithm
			// must have found it and therefore it cannot be busy either.
			unmap_page(area, address);
		}

		if (mapPage) {
#if defined(CONFIG_MEMORY_HOTPLUG)
			if (map_page(area, context.page, address, newProtection, &context.system_reservation) != 0)
#else
			if (map_page(area, context.page, address, newProtection, &context.reservation) != 0)
#endif
			{
				// Mapping can only fail, when the page mapping object couldn't
				// be allocated. Save for the missing mapping everything is
				// fine, though. If this was a regular page fault, we'll simply
				// leave and probably fault again. To make sure we'll have more
				// luck then, we ensure that the minimum object reserve is
				// available.
				context.UnlockAll();

				if(object_cache_reserve(gPageMappingsObjectCache, 1, CACHE_DONT_OOM_KILL) != 0) {
					status = -ENOMEM;
				} else {
					if (wirePage != nullptr) {
						// The caller expects us to wire the page. Since
						// object_cache_reserve() succeeded, we should now be able
						// to allocate a mapping structure. Restart.
						continue;
					}
				}

				break;
			}
		} else if(context.page->state == PAGE_STATE_INACTIVE) {
			vm_page_set_state(context.page, PAGE_STATE_ACTIVE);
		}

		// also wire the page, if requested
		if (wirePage != nullptr && status == 0) {
			increment_page_wired_count(context.page);
			*wirePage = context.page;
		}

		break;
	}

	return status;
}

int vm_page_fault(uintptr_t address, bool isWrite, bool isExecute, bool isUser)
{
	struct thread * thread  = get_current_thread();
	struct task * team = thread->task;
	VMAddressSpace * space;

	if(address <= team->addressSpace->EndAddress())
		space = team->addressSpace;
	else if(address >= VM_KERNEL_SPACE_BASE)
		space = VMAddressSpace::Kernel();
	else
		return -EFAULT;

	return vm_soft_fault(space, address, isWrite, isExecute, isUser, nullptr);
}


/*!	Creates a new cache on top of given cache, moves all areas from
	the old cache to the new one, and changes the protection of all affected
	areas' pages to read-only. If requested, wired pages are moved up to the
	new cache and copies are added to the old cache in their place.
	Preconditions:
	- The given cache must be locked.
	- All of the cache's areas' address spaces must be read locked.
	- Either the cache must not have any wired ranges or a page reservation for
	  all wired pages must be provided, so they can be copied.

	\param lowerCache The cache on top of which a new cache shall be created.
	\param wiredPagesReservation If \c NULL there must not be any wired pages
		in \a lowerCache. Otherwise as many pages must be reserved as the cache
		has wired page. The wired pages are copied in this case.
*/
static int vm_copy_on_write_area(VMCache* lowerCache,
	vm_reservation_t * wiredPagesReservation,
	uint32_t allocationFlags)
{
	VMCache* upperCache;

	// We need to separate the cache from its areas. The cache goes one level
	// deeper and we create a new cache inbetween.

	// create an anonymous cache
	int status = VMCacheFactory::CreateAnonymousCache(upperCache, false, 0,
		lowerCache->GuardSize() / PAGE_SIZE,
		dynamic_cast<VMAnonymousNoSwapCache*>(lowerCache) == NULL,
		GFP_PRIO_USER,
		allocationFlags);

	if (status != 0)
		return status;

	upperCache->Lock();

	upperCache->temporary = true;
	upperCache->virtual_base = lowerCache->virtual_base;
	upperCache->virtual_end = lowerCache->virtual_end;

	// transfer the lower cache areas to the upper cache
	upperCache->TransferAreas(lowerCache);

	lowerCache->AddConsumer(upperCache);

	// We now need to remap all pages from all of the cache's areas read-only,
	// so that a copy will be created on next write access. If there are wired
	// pages, we keep their protection, move them to the upper cache and create
	// copies for the lower cache.
	if (wiredPagesReservation != nullptr) {
		// We need to handle wired pages -- iterate through the cache's pages.
		for (VMCachePagesTree::Iterator it = lowerCache->pages.GetIterator();
				struct page * page = it.Next();) {
			if (page->WiredCount() > 0) {
				// allocate a new page and copy the wired one
				struct page* copiedPage = vm_page_allocate_page_etc(wiredPagesReservation, PAGE_STATE_ACTIVE);

				vm_memcpy_physical_page(page_to_phys(copiedPage), page_to_phys(page));

				// move the wired page to the upper cache (note: removing is OK
				// with the SplayTree iterator) and insert the copy
				upperCache->MovePage(page);
				lowerCache->InsertPage(copiedPage, page->cache_offset * PAGE_SIZE);
			} else {
				// Change the protection of this page in all areas.
				for (VMArea * tempArea = upperCache->areas; tempArea != nullptr;
						tempArea = tempArea->cache_next) {
					// The area must be readable in the same way it was
					// previously writable.
					uint32_t protection = B_KERNEL_READ_AREA;
					if ((tempArea->protection & B_READ_AREA) != 0)
						protection |= B_READ_AREA;

					VMTranslationMap* map = tempArea->address_space->TranslationMap();
					map->Lock();
					map->ProtectPage(tempArea, virtual_page_address(tempArea, page), protection);
					map->Unlock();
				}
			}
		}
	} else {
		ASSERT(lowerCache->wired_pages_count == 0);

		// just change the protection of all areas
		for (VMArea * tempArea = upperCache->areas; tempArea != nullptr;
				tempArea = tempArea->cache_next) {
			// The area must be readable in the same way it was previously
			// writable.
			uint32_t protection = B_KERNEL_READ_AREA;
			if ((tempArea->protection & B_READ_AREA) != 0)
				protection |= B_READ_AREA;

			VMTranslationMap* map = tempArea->address_space->TranslationMap();
			map->Lock();
			map->ProtectArea(tempArea, protection);
			map->Unlock();
		}
	}

	vm_area_put_locked_cache(upperCache);

	return 0;
}

int vm_set_area_protection(pid_t team, area_id areaID, uint32_t newProtection, bool kernel)
{
	fix_protection(&newProtection);

	if (!arch_vm_supports_protection(newProtection))
		return -EINVAL;

	if(newProtection & B_WRITE_AREA)
		newProtection |= B_KERNEL_WRITE_AREA;
	if(newProtection & B_READ_AREA)
		newProtection |= B_KERNEL_READ_AREA;

	bool becomesWritable
		= (newProtection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0;

	// lock address spaces and cache
	MultiAddressSpaceLocker locker;
	VMCache* cache;
	VMArea * area;
	int status;
	AreaCacheLocker cacheLocker;
	bool isWritable;
	uint32_t allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY;

	bool restart;
	do {
		restart = false;

		locker.Unset();
		status = locker.AddAreaCacheAndLock(areaID, true, false, area, &cache);
		if (status != 0)
			return status;

		cacheLocker.SetTo(cache, true);	// already locked

		if (!kernel && (area->address_space == VMAddressSpace::Kernel()
				|| (area->protection & B_KERNEL_AREA) != 0)) {
			printk(KERN_ERR|WITHUART, "vm_set_area_protection: team %" PRId32 " tried to "
				"set protection %#" PRIx32 " on kernel area %" PRId32
				" (%s)\n", team, newProtection, areaID, area->name);
			return -EPERM;
		}

		if (!kernel && area->protection_max != 0
			&& (newProtection & area->protection_max)
				!= (newProtection & B_USER_PROTECTION)) {
			printk(KERN_ERR|WITHUART, "vm_set_area_protection: team %" PRId32 " tried to "
				"set protection %#" PRIx32 " (max %#" PRIx32 ") on kernel "
				"area %" PRId32 " (%s)\n", team, newProtection,
				area->protection_max, areaID, area->name);
			return -EPERM;
		}

		if(area->address_space == VMAddressSpace::Kernel()) {
			allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
		} else {
			allocationFlags = 0;
		}

		if ((area->protection & VMA_FLAG_PERM_MASK) == (newProtection & VMA_FLAG_PERM_MASK))
			return 0;

		if (team != VMAddressSpace::KernelID() && area->address_space->ID() != team) {
			// unless you're the kernel, you are only allowed to set
			// the protection of your own areas
			return -EPERM;
		}

		isWritable = (area->protection & (VMA_FLAG_U_WRITE | VMA_FLAG_S_WRITE)) != 0;

		// Make sure the area (respectively, if we're going to call
		// vm_copy_on_write_area(), all areas of the cache) doesn't have any
		// wired ranges.
		if (!isWritable && becomesWritable && !cache->consumers.IsEmpty()) {
			for (VMArea * otherArea = cache->areas; otherArea != nullptr;
					otherArea = otherArea->cache_next) {
				if (wait_if_area_is_wired(otherArea, &locker, &cacheLocker)) {
					restart = true;
					break;
				}
			}
		} else {
			if (wait_if_area_is_wired(area, &locker, &cacheLocker))
				restart = true;
		}
	} while (restart);

	bool changePageProtection = true;
	bool changeTopCachePagesOnly = false;

	newProtection = (area->protection & ~VMA_FLAG_PERM_MASK) | (newProtection & VMA_FLAG_PERM_MASK);

	if (area->cache_type >= CACHE_TYPE_DEVICE) {
		// Don't do anything special. Just update protection
	} else if (isWritable && !becomesWritable) {
		// writable -> !writable

		if (cache->source != nullptr && cache->temporary) {
			if (cache->CountWritableAreas(area) == 0) {
				// Since this cache now lives from the pages in its source cache,
				// we can change the cache's commitment to take only those pages
				// into account that really are in this cache.

				status = cache->Commit(cache->page_count * PAGE_SIZE,
					area->address_space == VMAddressSpace::Kernel()
						? GFP_PRIO_SYSTEM : GFP_PRIO_USER);

				// TODO: we may be able to join with our source cache, if
				// count == 0
			}
		}

		// If only the writability changes, we can just remap the pages of the
		// top cache, since the pages of lower caches are mapped read-only
		// anyway. That's advantageous only, if the number of pages in the cache
		// is significantly smaller than the number of pages in the area,
		// though.
		if ((newProtection & VMA_FLAG_PERM_MASK)
				== ((area->protection & ~(VMA_FLAG_U_WRITE | VMA_FLAG_S_WRITE)) & VMA_FLAG_PERM_MASK)
			&& cache->page_count * 2 < area->Size() / PAGE_SIZE) {
			changeTopCachePagesOnly = true;
		}
	} else if (!isWritable && becomesWritable) {
		// !writable -> writable

		if (!cache->consumers.IsEmpty()) {
			// There are consumers -- we have to insert a new cache. Fortunately
			// vm_copy_on_write_area() does everything that's needed.
			changePageProtection = false;
			status = vm_copy_on_write_area(cache, nullptr, allocationFlags);
		} else {
			// No consumers, so we don't need to insert a new one.
			if (cache->source != nullptr && cache->temporary) {
				// the cache's commitment must contain all possible pages
				status = cache->Commit(cache->virtual_end - cache->virtual_base,
						area->address_space == VMAddressSpace::Kernel() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER);
			}

			if (status == 0 && cache->source != nullptr) {
				// There's a source cache, hence we can't just change all pages'
				// protection or we might allow writing into pages belonging to
				// a lower cache.
				changeTopCachePagesOnly = true;
			}
		}
	} else {
		// we don't have anything special to do in all other cases
	}

	if (status == 0) {
		// remap existing pages in this cache
		if (changePageProtection) {
			VMTranslationMap* map = area->address_space->TranslationMap();
			map->Lock();

			if (changeTopCachePagesOnly) {
				off_t firstPageOffset = area->cache_offset / PAGE_SIZE;
				off_t lastPageOffset = firstPageOffset + area->Size() / PAGE_SIZE;
				for (VMCachePagesTree::Iterator it = cache->pages.GetIterator();
						struct page * page = it.Next();) {
					if (page->cache_offset >= firstPageOffset
						&& page->cache_offset <= lastPageOffset) {
						uintptr_t address = virtual_page_address(area, page);

						map->ProtectPage(area, address, newProtection);
					}
				}
			} else {
				map->ProtectArea(area, newProtection);
			}

			map->Unlock();
		}

		area->protection = newProtection;
	}

	return status;
}


int vm_copy_area(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, area_id sourceID)
{
	// Do the locking: target address space, all address spaces associated with
	// the source cache, and the cache itself.
	MultiAddressSpaceLocker locker;
	VMAddressSpace * targetAddressSpace;
	VMCache* cache;
	VMArea * source;
	AreaCacheLocker cacheLocker;
	int status;
	bool sharedArea;

	size_t wiredPages = 0;
	vm_reservation_t wiredPagesReservation;

	bool restart;
	do {
		restart = false;

		locker.Unset();
		status = locker.AddTeam(team, true, &targetAddressSpace);
		if (status == 0) {
			status = locker.AddAreaCacheAndLock(sourceID, false, false, source,
				&cache);
		}
		if (status != 0)
			return status;

		cacheLocker.SetTo(cache, true);	// already locked

		sharedArea = (source->protection & B_SHARED_AREA) != 0;

		int areaGFP = GFP_PRIO_USER;
		size_t oldWiredPages = wiredPages;
		wiredPages = 0;

		// If the source area isn't shared, count the number of wired pages in
		// the cache and reserve as many pages.
		if (!sharedArea) {
			wiredPages = cache->wired_pages_count;

			if (wiredPages > oldWiredPages) {
				cacheLocker.Unlock();
				locker.Unlock();

				if (oldWiredPages > 0)
					vm_page_unreserve_pages(&wiredPagesReservation);

				if(!vm_reserve_pages(&wiredPagesReservation,
						wiredPages,
						areaGFP,
						false,
						false))
				{
					return -ENOMEM;
				}

				restart = true;
			}
		} else if (oldWiredPages > 0)
			vm_page_unreserve_pages(&wiredPagesReservation);
	} while (restart);

	// unreserve pages later
	struct PagesUnreserver {
		PagesUnreserver(vm_reservation_t* reservation)
			:
			fReservation(reservation)
		{
		}

		~PagesUnreserver()
		{
			if (fReservation != nullptr)
				vm_page_unreserve_pages(fReservation);
		}

	private:
		vm_reservation_t*	fReservation;
	} pagesUnreserver(wiredPages > 0 ? &wiredPagesReservation : nullptr);

	if (source->protection & B_NOCOPY_AREA)
		return -EINVAL;

	bool writableCopy = (source->protection
			& (B_KERNEL_WRITE_AREA | B_WRITE_AREA)) != 0;
	uint8_t *targetPageProtections = nullptr;

	if (source->page_protections != NULL) {
		size_t bytes = (source->Size() / PAGE_SIZE + 1) / 2;
		targetPageProtections = (uint8_t*) malloc_etc(bytes, CACHE_DONT_LOCK_KERNEL);

		if (targetPageProtections == NULL)
			return -ENOMEM;

		memcpy(targetPageProtections, source->page_protections, bytes);

		if (!writableCopy) {
			for (size_t i = 0; i < bytes; i++) {
				if ((targetPageProtections[i]
						& (B_WRITE_AREA | (B_WRITE_AREA << 4))) != 0) {
					writableCopy = true;
					break;
				}
			}
		}
	}

	if (addressSpec == B_CLONE_ADDRESS) {
		addressSpec = B_EXACT_ADDRESS;
		*_address = source->Base();
	}

	// First, create a cache on top of the source area, respectively use the
	// existing one, if this is a shared area.

	VMArea * target = nullptr;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)*_address;
	addressRestrictions.address_specification = addressSpec;

	uint32_t mallocFlags = CACHE_DONT_WAIT_FOR_MEMORY;

	if(targetAddressSpace == VMAddressSpace::Kernel()) {
		mallocFlags = CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY;
	} else {
		mallocFlags = 0;
	}

	status = map_backing_store(targetAddressSpace, cache, source->cache_offset,
			name, source->Size(), source->wiring, source->protection,
			source->protection_max, writableCopy ? 0 : CREATE_AREA_DONT_COMMIT_MEMORY,
			sharedArea ? false : true, &addressRestrictions, true, &target,
			_address, mallocFlags);

	if (status < 0) {
		free_etc(targetPageProtections, CACHE_DONT_LOCK_KERNEL);
		return status;
	}

	if (targetPageProtections) {
		target->page_protections = targetPageProtections;
	}

	if (sharedArea) {
		// The new area uses the old area's cache, but map_backing_store()
		// hasn't acquired a ref. So we have to do that now.
		cache->AcquireRefLocked();
	}

	// If the source area is writable, we need to move it one layer up as well

	if (!sharedArea) {
		if (writableCopy) {
			// TODO: do something more useful if this fails!
			status = vm_copy_on_write_area(cache,
					wiredPages > 0 ? &wiredPagesReservation : nullptr,
					mallocFlags);

			if(status < 0) {
				delete_area(targetAddressSpace, target, false);
				return status;
			}
		}
	}

	if (sharedArea &&
		((source->protection & B_COPYONCE_AREA) != 0))
	{
		source->protection |= B_NOCOPY_AREA;
	}

	// we return the ID of the newly created area
	return target->id;
}

/*!	\a cache must be locked. The area's address space must be read-locked.
*/
static void pre_map_area_pages(VMArea * area, VMCache* cache,
	vm_reservation_t* reservation)
{
	uintptr_t baseAddress = area->Base();
	off_t cacheOffset = area->cache_offset;
	off_t firstPage = cacheOffset / PAGE_SIZE;
	off_t endPage = firstPage + area->Size() / PAGE_SIZE;

	for (VMCachePagesTree::Iterator it
				= cache->pages.GetIterator(firstPage, true, true);
			struct page * page = it.Next();) {
		if (page->cache_offset >= endPage)
			break;

		// skip busy and inactive pages
		if (page->busy || page->usage_count == 0)
			continue;

		map_page(area, page,
			baseAddress + (page->cache_offset * PAGE_SIZE - cacheOffset),
			B_READ_AREA | B_KERNEL_READ_AREA,
			reservation);
	}
}

/*!	Will map the file specified by \a fd to an area in memory.
	The file will be mirrored beginning at the specified \a offset. The
	\a offset and \a size arguments have to be page aligned.
*/
static int _vm_map_file(pid_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t size, uint32_t protection, bool privateMapping,
	bool unmapAddressRange, int fd, off_t offset, bool kernel)
{
	int status;

	size = ROUND_PGUP(size);

	if (offset % PAGE_SIZE)
		return -EINVAL;

	if (!privateMapping)
		protection |= B_SHARED_AREA;

	if (addressSpec != B_EXACT_ADDRESS)
		unmapAddressRange = false;

	if (fd < 0) {
		uint32_t flags = unmapAddressRange ? CREATE_AREA_UNMAP_ADDRESS_RANGE : 0;
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = (void *)*_address;
		virtualRestrictions.address_specification = addressSpec;
		physical_address_restrictions physicalRestrictions = {};

		return vm_create_anonymous_area_etc(team, name, size, B_NO_LOCK, protection,
				flags, 0, &virtualRestrictions, &physicalRestrictions,
				kernel, _address);
	}

	// get the open flags of the FD
	file_descriptor* descriptor = get_fd(get_current_io_context(kernel), fd);
	if (descriptor == nullptr)
		return -EBADF;
	int32_t openMode = descriptor->open_mode;
	std::unique_ptr<file_descriptor, decltype(&put_fd)> decriptorPutter(descriptor, put_fd);

	// The FD must open for reading at any rate. For shared mapping with write
	// access, additionally the FD must be open for writing.
	if ((openMode & VFS_O_ACCMODE) == VFS_O_WRONLY
		|| (!privateMapping
			&& (protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0
			&& (openMode & VFS_O_ACCMODE) == VFS_O_RDONLY))
	{
		return -EACCES;
	}

	uint32_t protectionMax = 0;
	if (!privateMapping) {
		protectionMax = protection | B_READ_AREA;
		if ((openMode & O_ACCMODE) == O_RDWR) {
			protectionMax |= B_WRITE_AREA;
		}
	}

	// get the vnode for the object, this also grabs a ref to it
	struct vnode* vnode = nullptr;
	status = vfs_get_vnode_from_fd(fd, kernel, &vnode);
	if (status < 0)
		return status;
	CObjectDeleter<struct vnode> vnodePutter(vnode, vfs_put_vnode);

	// If we're going to pre-map pages, we need to reserve the pages needed by
	// the mapping backend upfront.
	size_t reservedPreMapPages = 0;
	vm_reservation_t reservation;
	if ((protection & B_READ_AREA) != 0) {
		AddressSpaceWriteLocker locker;
		status = locker.SetTo(team);
		if (status != 0) {
			return status;
		}

		reservedPreMapPages = locker.AddressSpace()->TranslationMap()->MaxPagesNeededToMap(0, size);

		locker.Unlock();

		if(!vm_reserve_pages(&reservation, reservedPreMapPages,
				team == VMAddressSpace::KernelID() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER,
				false, false))
		{
			return -ENOMEM;
		}
	}

	struct PageUnreserver {
		PageUnreserver(vm_reservation_t * reservation)
			:
			fReservation(reservation)
		{
		}

		~PageUnreserver()
		{
			if (fReservation != nullptr)
				vm_page_unreserve_pages(fReservation);
		}

		vm_reservation_t* fReservation;
	} pageUnreserver(reservedPreMapPages > 0 ? &reservation : nullptr);

	// Lock the address space and, if the specified address range shall be
	// unmapped, ensure it is not wired.
	AddressSpaceWriteLocker locker;
	do {
		if (locker.SetTo(team) != 0)
			return -ESRCH;
	} while (unmapAddressRange
		&& wait_if_address_range_is_wired(locker.AddressSpace(), *_address, size, &locker));

	// TODO: this only works for file systems that use the file cache
	VMCache* cache = nullptr;
	status = vfs_get_vnode_cache(vnode, &cache, false);
	if (status < 0) {
		if(status != -EINVAL) {
			return status;
		}

		if(vfs_can_page(vnode, descriptor->cookie)) {
			// Underlying filesystem allows paging
			status = vfs_get_vnode_cache(vnode, &cache, true);

			if (status < 0) {
				return status == -EINVAL ? -ENODEV : status;
			}
		} else {
			return -ENODEV;
		}
	}

	cache->Lock();

	VMArea * area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void *)*_address;
	addressRestrictions.address_specification = addressSpec;

	uint32_t mallocFlags = CACHE_DONT_WAIT_FOR_MEMORY;

	if(locker.AddressSpace() == VMAddressSpace::Kernel()) {
		mallocFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;
	} else {
		mallocFlags = 0;
	}

	status = map_backing_store(locker.AddressSpace(), cache, offset, name, size,
		B_NO_LOCK, protection, protectionMax, unmapAddressRange ? CREATE_AREA_UNMAP_ADDRESS_RANGE : 0,
		privateMapping, &addressRestrictions, kernel, &area, _address, mallocFlags);

	if (status < 0 || privateMapping) {
		// map_backing_store() cannot know we no longer need the ref
		cache->ReleaseRefLocked();
	}

	if (status == 0 && (protection & B_READ_AREA) != 0)
		pre_map_area_pages(area, cache, &reservation);

	cache->Unlock();

	if (status != 0)
		return status;

	area->cache_type = CACHE_TYPE_VNODE;
	return area->id;
}

int vm_map_file(pid_t aid, const char* name, vaddr_t * address, uint32_t addressSpec,
	size_t size, uint32_t protection, bool privateMapping, bool unmapAddressRange,
	int fd, off_t offset)
{
	if (!arch_vm_supports_protection(protection))
		return -EINVAL;

	return _vm_map_file(aid, name, address, addressSpec, size, protection,
			privateMapping, unmapAddressRange, fd, offset, true);
}

int user_map_file(const char* name, vaddr_t * _address, uint32_t addressSpec,
	size_t size, uint32_t protection, bool privateMapping, bool unmapAddressRange,
	int fd, off_t offset)
{
	if (!arch_vm_supports_protection(protection))
		return -EINVAL;

	return _vm_map_file(VMAddressSpace::CurrentID(), name, _address, addressSpec, size, protection, privateMapping,
			unmapAddressRange, fd, offset, false);
}

int vm_wire_page(pid_t team, uintptr_t address, bool writable, struct VMPageWiringInfo* info)
{
	uintptr_t pageAddress = ROUND_PGDOWN((uintptr_t)address);

	info->range.SetTo(pageAddress, PAGE_SIZE, writable, false);

	// compute the page protection that is required
	bool isUser = address < VM_PROC_LIMIT;
	uint32_t requiredProtection = PAGE_PRESENT
		| B_KERNEL_READ_AREA | (isUser ? B_READ_AREA : 0);
	if (writable)
		requiredProtection |= B_KERNEL_WRITE_AREA | (isUser ? B_WRITE_AREA : 0);

	// get and read lock the address space
	VMAddressSpace* addressSpace = nullptr;
	if (isUser) {
		if (team == B_CURRENT_TEAM) {
			addressSpace = VMAddressSpace::GetCurrent();
		} else {
			addressSpace = VMAddressSpace::Get(team);
			if (addressSpace == nullptr)
				return -ESRCH;
		}
	} else {
		addressSpace = VMAddressSpace::GetKernel();
	}

	AddressSpaceReadLocker addressSpaceLocker(addressSpace, true);

	VMTranslationMap* map = addressSpace->TranslationMap();
	int error = 0;

	if(map->VirtualAccessedBit()) {
		requiredProtection |= PAGE_ACCESSED;
	}

	// get the area
	VMArea* area = addressSpace->LookupArea(address);
	if (area == nullptr || !area->cache) {
		addressSpace->Put();
		return -EFAULT;
	}

	if(area->protection & B_KERNEL_MEMORY_TYPE_MASK) {
		// We can't obtain page from such mappings
		addressSpace->Put();
		return -EFAULT;
	}

	// Lock the area's top cache. This is a requirement for VMArea::Wire().
	VMCacheChainLocker cacheChainLocker(vm_area_get_locked_cache(area));

	// mark the area range wired
	area->Wire(&info->range);

	// Lock the area's cache chain and the translation map. Needed to look
	// up the page and play with its wired count.
	cacheChainLocker.LockAllSourceCaches();
	map->Lock();

	vm_paddr_t physicalAddress;
	uint32_t flags;
	struct page * page;

	if (map->Query(pageAddress, &physicalAddress, &flags) == 0
		&& (flags & requiredProtection) == requiredProtection
		&& (page = phys_to_page(physicalAddress)) != nullptr
		)
	{
		// Already mapped with the correct permissions -- just increment
		// the page's wired count.
		increment_page_wired_count(page);

		map->Unlock();
		cacheChainLocker.Unlock();
		addressSpaceLocker.Unlock();
	} else {
		// Let vm_soft_fault() map the page for us, if possible. We need
		// to fully unlock to avoid deadlocks. Since we have already
		// wired the area itself, nothing disturbing will happen with it
		// in the meantime.
		map->Unlock();
		cacheChainLocker.Unlock();
		addressSpaceLocker.Unlock();

		error = vm_soft_fault(addressSpace, pageAddress, writable, false,
			isUser, &page);

		if (error != 0) {
			// The page could not be mapped -- clean up.
			VMCache* cache = vm_area_get_locked_cache(area);
			area->Unwire(&info->range);
			cache->ReleaseRefAndUnlock();
			addressSpace->Put();
			return error;
		}
	}

	info->physicalAddress = page_to_phys(page) | (address & (PAGE_SIZE - 1));
	info->page = page;

	return 0;
}

void vm_unwire_page(VMPageWiringInfo* info)
{
	// lock the address space
	VMArea * area = info->range.area;
	AddressSpaceReadLocker addressSpaceLocker(area->address_space, false);
		// takes over our reference

	// lock the top cache
	VMCache* cache = vm_area_get_locked_cache(area);
	VMCacheChainLocker cacheChainLocker(cache);

	if (info->page->Cache() != cache) {
		// The page is not in the top cache, so we lock the whole cache chain
		// before touching the page's wired count.
		cacheChainLocker.LockAllSourceCaches();
	}

	decrement_page_wired_count(info->page);

	// remove the wired range from the range
	area->Unwire(&info->range);

	cacheChainLocker.Unlock();
}

int lock_memory_etc(pid_t team, void* address, size_t numBytes, bool writable)
{
	uintptr_t lockBaseAddress = rounddown2((uintptr_t)address, PAGE_SIZE);
	uintptr_t lockEndAddress = roundup2((uintptr_t)address + numBytes, PAGE_SIZE);

	// Ensure numBytes wasn't passed a negative value
	if ((uintptr_t)address + numBytes < (uintptr_t)address)
		return -EFAULT;

	// compute the page protection that is required
	bool isUser = (uintptr_t)address < VM_PROC_LIMIT;
	uint32_t requiredProtection = PAGE_PRESENT
		| B_KERNEL_READ_AREA | (isUser ? B_READ_AREA : 0);
	if (writable)
		requiredProtection |= B_KERNEL_WRITE_AREA | (isUser ? B_WRITE_AREA : 0);

	uint32_t mallocFlags = isUser ? 0 : (CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL);

	// get and read lock the address space
	VMAddressSpace* addressSpace = nullptr;
	if (isUser) {
		if (team == B_CURRENT_TEAM) {
			addressSpace = VMAddressSpace::GetCurrent();
		} else {
			addressSpace = VMAddressSpace::Get(team);
			if (addressSpace == nullptr)
				return -ESRCH;
		}
	} else {
		addressSpace = VMAddressSpace::GetKernel();
	}

	AddressSpaceReadLocker addressSpaceLocker(addressSpace, true);

		// We get a new address space reference here. The one we got above will
		// be freed by unlock_memory_etc().

	VMTranslationMap* map = addressSpace->TranslationMap();
	int error = 0;

	if(map->VirtualAccessedBit()) {
		requiredProtection |= PAGE_ACCESSED;
	}

	// iterate through all concerned areas
	uintptr_t nextAddress = lockBaseAddress;
	while (nextAddress != lockEndAddress) {
		// get the next area
		VMArea* area = addressSpace->LookupArea(nextAddress);
		if (area == nullptr || !area->cache) {
			error = -EFAULT;
			break;
		}

		uintptr_t areaStart = nextAddress;
		uintptr_t areaEnd = MIN(lockEndAddress, area->End());

		// allocate the wired range (do that before locking the cache to avoid
		// deadlocks)
		VMAreaWiredRange* range = new(std::nothrow, wait_flags_t(mallocFlags))
			VMAreaWiredRange(areaStart, areaEnd - areaStart, writable, true);

		if (range == nullptr) {
			error = -ENOMEM;
			break;
		}

		// Lock the area's top cache. This is a requirement for VMArea::Wire().
		VMCacheChainLocker cacheChainLocker(vm_area_get_locked_cache(area));

		// mark the area range wired
		area->Wire(range);

		// Depending on the area cache type and the wiring, we may not need to
		// look at the individual pages.
		if (area->cache_type > CACHE_TYPE_VNODE
			|| area->wiring == B_FULL_LOCK
			|| area->wiring == B_CONTIGUOUS)
		{
			nextAddress = areaEnd;
			continue;
		}

		// Lock the area's cache chain and the translation map. Needed to look
		// up pages and play with their wired count.
		cacheChainLocker.LockAllSourceCaches();
		map->Lock();

		// iterate through the pages and wire them
		for (; nextAddress != areaEnd; nextAddress += PAGE_SIZE) {
			vm_paddr_t physicalAddress;
			uint32_t flags;
			struct page* page;

			if (map->Query(nextAddress, &physicalAddress, &flags) == 0
				&& (flags & requiredProtection) == requiredProtection
				&& (page = phys_to_page(physicalAddress)) != nullptr)
			{
				// Already mapped with the correct permissions -- just increment
				// the page's wired count.
				increment_page_wired_count(page);
			} else {
				// Let vm_soft_fault() map the page for us, if possible. We need
				// to fully unlock to avoid deadlocks. Since we have already
				// wired the area itself, nothing disturbing will happen with it
				// in the meantime.
				map->Unlock();
				cacheChainLocker.Unlock();
				addressSpaceLocker.Unlock();

				error = vm_soft_fault(addressSpace, nextAddress, writable,
					false, isUser, &page);

				addressSpaceLocker.Lock();
				cacheChainLocker.SetTo(vm_area_get_locked_cache(area));
				cacheChainLocker.LockAllSourceCaches();
				map->Lock();
			}

			if (error != 0)
				break;
		}

		map->Unlock();

		if (error == 0) {
			cacheChainLocker.Unlock();
		} else {
			// An error occurred, so abort right here. If the current address
			// is the first in this area, unwire the area, since we won't get
			// to it when reverting what we've done so far.
			if (nextAddress == areaStart) {
				area->Unwire(range);
				cacheChainLocker.Unlock();
				range->~VMAreaWiredRange();
				free_etc(range, mallocFlags);
			} else {
				// Patch the failed range so we don't panic
				range->size = nextAddress - areaStart;

				cacheChainLocker.Unlock();
			}

			break;
		}
	}

	if (error != 0) {
		// An error occurred, so unwire all that we've already wired. Note that
		// even if not a single page was wired, unlock_memory_etc() is called
		// to put the address space reference.
		addressSpaceLocker.Unlock();
		unlock_memory_etc(team, (void*)lockBaseAddress, nextAddress - lockBaseAddress, writable);
	}

	return error;
}


int lock_memory(void* address, size_t numBytes, bool writable)
{
	return lock_memory_etc(B_CURRENT_TEAM, address, numBytes, writable);
}

int unlock_memory_etc(pid_t team, void* address, size_t numBytes, bool writable)
{
	uintptr_t lockBaseAddress = rounddown2((uintptr_t)address, PAGE_SIZE);
	uintptr_t lockEndAddress = roundup2((uintptr_t)address + numBytes, PAGE_SIZE);

	// compute the page protection that is required
	bool isUser = (uintptr_t)address < VM_PROC_LIMIT;
	uint32_t mallocFlags = isUser ? 0 : (CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL);

	// get and read lock the address space
	VMAddressSpace* addressSpace = nullptr;
	if (isUser) {
		if (team == B_CURRENT_TEAM) {
			addressSpace = VMAddressSpace::GetCurrent();
		} else {
			addressSpace = VMAddressSpace::Get(team);
			if (addressSpace == nullptr)
				return -ESRCH;
		}
	} else {
		addressSpace = VMAddressSpace::GetKernel();
	}

	AddressSpaceReadLocker addressSpaceLocker(addressSpace, false);
		// Take over the address space reference. We don't unlock until we're
		// done.

	VMTranslationMap* map = addressSpace->TranslationMap();
	int error = 0;

	// iterate through all concerned areas
	uintptr_t nextAddress = lockBaseAddress;
	while (nextAddress != lockEndAddress) {
		// get the next area
		VMArea* area = addressSpace->LookupArea(nextAddress);
		if (area == nullptr || !area->cache) {
			error = -EFAULT;
			break;
		}

		uintptr_t areaStart = nextAddress;
		uintptr_t areaEnd = MIN(lockEndAddress, area->End());

		// Lock the area's top cache. This is a requirement for
		// VMArea::Unwire().
		VMCacheChainLocker cacheChainLocker(vm_area_get_locked_cache(area));

		// Depending on the area cache type and the wiring, we may not need to
		// look at the individual pages.
		if (area->cache_type > CACHE_TYPE_VNODE
			|| area->wiring == B_FULL_LOCK
			|| area->wiring == B_CONTIGUOUS) {
			// unwire the range (to avoid deadlocks we delete the range after
			// unlocking the cache)
			nextAddress = areaEnd;
			VMAreaWiredRange* range = area->Unwire(areaStart,
				areaEnd - areaStart, writable);
			cacheChainLocker.Unlock();
			range->~VMAreaWiredRange();
			free_etc(range, mallocFlags);
			continue;
		}

		// Lock the area's cache chain and the translation map. Needed to look
		// up pages and play with their wired count.
		cacheChainLocker.LockAllSourceCaches();
		map->Lock();

		// iterate through the pages and unwire them
		for (; nextAddress != areaEnd; nextAddress += PAGE_SIZE) {
			vm_paddr_t physicalAddress;
			uint32_t flags;

			struct page * page;
			if (map->Query(nextAddress, &physicalAddress, &flags) == 0
				&& (flags & PAGE_PRESENT) != 0
				&& (page = phys_to_page(physicalAddress)) != nullptr)
			{
				// Already mapped with the correct permissions -- just increment
				// the page's wired count.
				decrement_page_wired_count(page);
			} else {
				panic("unlock_memory_etc(): Failed to unwire page: address space %p, address: %p", addressSpace,
					(void *)nextAddress);
			}
		}

		map->Unlock();

		// All pages are unwired. Remove the area's wired range as well (to
		// avoid deadlocks we delete the range after unlocking the cache).
		VMAreaWiredRange* range = area->Unwire(areaStart,
			areaEnd - areaStart, writable);

		cacheChainLocker.Unlock();

		range->~VMAreaWiredRange();
		free_etc(range, mallocFlags);
	}

	// get rid of the address space reference lock_memory_etc() acquired
	addressSpace->Put();

	return error;
}

int unlock_memory(void* address, size_t numBytes, bool writable)
{
	return unlock_memory_etc(B_CURRENT_TEAM, address, numBytes, writable);
}

static inline VMArea * lookup_area(VMAddressSpace* addressSpace, area_id id)
{
	return VMAreaHash::Lookup(id, addressSpace);
}

static bool can_clone_area(struct task * localTask, VMArea * area, uint32_t mode)
{
	if ((area->protection & B_KERNEL_AREA) == B_KERNEL_AREA)
		return false;

	if (localTask->effective_uid == 0)
		return true;

	if (localTask->permission & (TASK_CAPABILITY_TRUSTED_CONTROL | TASK_CAPABILITY_DEVICE_DRIVER))
		return true;

	int accessMode = 0;

	if(mode & B_READ_AREA)
		accessMode |= 4;
	if(mode & B_WRITE_AREA)
		accessMode |= 2;
	if(mode & B_EXECUTE_AREA)
		accessMode |= 1;

	if (check_access_permissions(accessMode, area->mode, area->owner_gid,
			area->owner_uid, "VMArea clone"))
		return false;

	return true;
}

int vm_clone_area(pid_t team, const char* name, vaddr_t * address,
	uint32_t addressSpec, uint32_t protection, bool privateMapping, int sourceID,
	bool kernel)
{
	VMArea* newArea = nullptr;
	VMArea* sourceArea;
	uint32_t mallocFlags = CACHE_DONT_WAIT_FOR_MEMORY;
	vm_reservation_t reservation;
	struct task * currentTask = team_get_current_team();

	// Check whether the source area exists and is cloneable. If so, mark it
	// B_SHARED_AREA, so that we don't get problems with copy-on-write.
	{
		AddressSpaceWriteLocker locker;
		int status = locker.SetFromArea(sourceID, sourceArea);
		if (status != 0)
			return status;

		if(!kernel && !can_clone_area(currentTask, sourceArea, protection))
			return -EPERM;

		sourceArea->protection |= B_SHARED_AREA;
		protection |= B_SHARED_AREA;

		protection = (protection & VMA_FLAG_PERM_MASK)
				| (sourceArea->protection & ~VMA_FLAG_PERM_MASK);
	}

	// Now lock both address spaces and actually do the cloning.

	MultiAddressSpaceLocker locker;
	VMAddressSpace* sourceAddressSpace;
	int status = locker.AddArea(sourceID, false, &sourceAddressSpace);
	if (status != 0)
		return status;

	VMAddressSpace* targetAddressSpace;
	status = locker.AddTeam(team, true, &targetAddressSpace);
	if (status != 0)
		return status;

	status = locker.Lock();
	if (status != 0)
		return status;

	sourceArea = lookup_area(sourceAddressSpace, sourceID);
	if (sourceArea == nullptr || !sourceArea->cache)
		return -EINVAL;

	if(!kernel && !can_clone_area(currentTask, sourceArea, protection))
		return -EPERM;

	if(targetAddressSpace == VMAddressSpace::Kernel() ||
			sourceAddressSpace == VMAddressSpace::Kernel()) {
		mallocFlags = CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY;
	} else {
		mallocFlags = 0;
	}

	VMCache* cache = vm_area_get_locked_cache(sourceArea);

	if (!kernel && sourceAddressSpace == VMAddressSpace::Kernel()
		&& targetAddressSpace != VMAddressSpace::Kernel()
		&& !(sourceArea->protection & B_USER_CLONEABLE_AREA)) {
		// kernel areas must not be cloned in userland, unless explicitly
		// declared user-cloneable upon construction
		status = -EPERM;
	} else if (sourceArea->cache_type == CACHE_TYPE_NULL) {
		// Can't clone NULL mapping
		status = -EPERM;
	} else {
		virtual_address_restrictions addressRestrictions = {};
		addressRestrictions.address = (void *)*address;
		addressRestrictions.address_specification = addressSpec;

		status = map_backing_store(targetAddressSpace, cache,
				sourceArea->cache_offset, name, sourceArea->Size(),
				sourceArea->wiring, protection, sourceArea->protection_max, 0,
				privateMapping, &addressRestrictions, kernel,
				&newArea, address, mallocFlags);
	}
	if (status == 0 && !privateMapping) {
		// If the mapping is REGION_PRIVATE_MAP, map_backing_store() needed
		// to create a new cache, and has therefore already acquired a reference
		// to the source cache - but otherwise it has no idea that we need
		// one.
		cache->AcquireRefLocked();
	}
	if (status == 0 && newArea->wiring == B_FULL_LOCK) {
		VMTranslationMap * map = newArea->address_space->TranslationMap();
		VMTranslationMap * sourceMap = sourceArea->address_space->TranslationMap();

		size_t reservePages = map->MaxPagesNeededToMap(newArea->Base(),
				newArea->Size());

		if (!vm_reserve_pages(&reservation, reservePages,
				newArea->address_space == VMAddressSpace::Kernel() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER,
				false, false))
				{
			status = -ENOMEM;
			cache->Unlock();
			delete_area(targetAddressSpace, newArea, false);
			cache->Lock();
		} else {
			// we need to map in everything at this point
			if (sourceArea->cache_type == CACHE_TYPE_DEVICE
					|| sourceArea->cache_type == CACHE_TYPE_NONSECURE_DEVICE) {
				sourceMap->Lock();

				vm_paddr_t physicalAddress;
				uint32_t oldProtection;
				sourceMap->Query(sourceArea->Base(), &physicalAddress,
						&oldProtection);

				sourceMap->Unlock();

				map->Lock();
				map->MapPhysical(newArea->Base(), physicalAddress, newArea->Size(), newArea->protection, &reservation);
				map->Unlock();
			} else if (sourceArea->cache_type >= CACHE_TYPE_NONSECURE) {
				for (uintptr_t base = 0; base < newArea->Size(); base += PAGE_SIZE) {
					vm_paddr_t physicalAddress;
					uint32_t oldProtection;

					sourceMap->Lock();
					sourceMap->Query(base + sourceArea->Base(), &physicalAddress, &oldProtection);
					sourceMap->Unlock();

					if(oldProtection & PAGE_PRESENT) {
						map->Lock();
						map->Map(base + newArea->Base(), physicalAddress, newArea->protection, &reservation);
						map->Unlock();
					}
				}
			} else {
				// map in all pages from source
				for (VMCachePagesTree::Iterator it = cache->pages.GetIterator();
						struct page * page = it.Next();) {
					if (!page->busy) {
						map_page(newArea, page,
								newArea->Base()
										+ ((page->cache_offset << PAGE_SHIFT)
												- newArea->cache_offset), protection,
								&reservation);
					}
				}
			}

			vm_page_unreserve_pages(&reservation);
		}
	}

	if (status == 0)
		newArea->cache_type = sourceArea->cache_type;

	vm_area_put_locked_cache(cache);

	if (status < 0)
		return status;

	return newArea->id;
}

/*!	Similar to get_memory_map(), but also allows to specify the address space
	for the memory in question and has a saner semantics.
	Returns \c 0 when the complete range could be translated or
	\c B_BUFFER_OVERFLOW, if the provided array wasn't big enough. In either
	case the actual number of entries is written to \c *_numEntries. Any other
	error case indicates complete failure; \c *_numEntries will be set to \c 0
	in this case.
*/
int get_memory_map_etc(pid_t team, const void* address, size_t numBytes,
		physical_entry * table, uint32_t * _numEntries)
{
	uint32_t numEntries = *_numEntries;
	*_numEntries = 0;

	VMAddressSpace* addressSpace;
	uintptr_t virtualAddress = (uintptr_t)address;
	uintptr_t pageOffset = virtualAddress & (PAGE_SIZE - 1);
	lpae_addr_t physicalAddress;
	int status = 0;
	int32_t index = -1;
	uintptr_t offset = 0;
	bool mayLock = are_interrupts_enabled();

	if (numEntries == 0 || numBytes == 0)
		return -EINVAL;

	// in which address space is the address to be found?
	if (virtualAddress < VM_PROC_LIMIT) {
		if(team == B_CURRENT_TEAM) {
			addressSpace = VMAddressSpace::GetCurrent();
		} else {
			addressSpace = VMAddressSpace::Get(team);
			if(!addressSpace)
				return -ESRCH;
		}
	} else {
		addressSpace = VMAddressSpace::GetKernel();
	}

	VMTranslationMap* map = addressSpace->TranslationMap();

	if(mayLock)
		map->Lock();

	while (offset < numBytes) {
		uintptr_t bytes = MIN(numBytes - offset, PAGE_SIZE);
		uint32_t flags;

		if (mayLock) {
			status = map->Query((uintptr_t)address + offset, &physicalAddress,
				&flags);
		} else {
			status = map->QueryInterrupt((uintptr_t)address + offset,
				&physicalAddress, &flags);
		}

		if (status < 0)
			break;

		if ((flags & PAGE_PRESENT) == 0) {
			panic("get_memory_map() called on unmapped memory!");
		}

		if ((flags & (B_NONSECURE_AREA | B_DEVICE_AREA)) != 0) {
			// We don't want such mappings
			return -EFAULT;
		}

		if (index < 0 && pageOffset > 0) {
//			physicalAddress += pageOffset;

			if (bytes > PAGE_SIZE - pageOffset)
				bytes = PAGE_SIZE - pageOffset;
		}

		// need to switch to the next physical_entry?
		if (index < 0 || table[index].address
				!= physicalAddress - table[index].size) {
			if ((uint32_t)++index + 1 > numEntries) {
				// table to small
				break;
			}
			table[index].address = physicalAddress;
			table[index].size = bytes;
		} else {
			// page does fit in current entry
			table[index].size += bytes;
		}

		offset += bytes;
	}

	if (mayLock)
		map->Unlock();

	addressSpace->Put();

	if (status != 0)
		return status;

	if ((uint32_t)index + 1 > numEntries) {
		*_numEntries = index;
		return -E2BIG;
	}

	*_numEntries = index + 1;
	return 0;
}

int32_t get_memory_map(const void* address, size_t numBytes, physical_entry* table, int32_t numEntries)
{
	uint32_t entriesRead = numEntries;
	int error = get_memory_map_etc(B_CURRENT_TEAM, address, numBytes, table, &entriesRead);
	if (error != 0)
		return error;
	// close the entry list
	// if it's only one entry, we will silently accept the missing ending
	if (numEntries == 1)
		return 0;
	if (entriesRead + 1 > (uint32_t)numEntries)
		return -E2BIG;
	table[entriesRead].address = 0;
	table[entriesRead].size = 0;
	return 0;
}

area_id find_area(const char* name)
{
	return VMAreaHash::Find(name);
}

int __sys_find_area(const char * uname)
{
	char name[B_OS_NAME_LENGTH];

	if(!uname || strlcpy_from_user(name, uname, B_OS_NAME_LENGTH) < 0)
		return -EFAULT;

	return find_area(name);
}


static int vm_resize_area(area_id areaID, size_t newSize, bool kernel)
{
	// is newSize a multiple of PAGE_SIZE?
	if (newSize & (PAGE_SIZE - 1))
		return -EINVAL;

	// lock all affected address spaces and the cache
	VMArea * area;
	VMCache* cache;
	MultiAddressSpaceLocker locker;
	AreaCacheLocker cacheLocker;

	int status;
	size_t oldSize;
	bool anyKernelArea;
	bool restart;

	do {
		anyKernelArea = false;
		restart = false;

		locker.Unset();

		status = locker.AddAreaCacheAndLock(areaID, true, true, area, &cache);
		if (status != 0)
			return status;
		cacheLocker.SetTo(cache, true);	// already locked

		// enforce restrictions
		if (!kernel && (area->address_space == VMAddressSpace::Kernel()
				|| (area->protection & B_KERNEL_AREA) != 0)) {
			printk(KERN_ERR|WITHUART, "vm_resize_area: team %" PRId32 " tried to "
				"resize kernel area %" PRId32 " (%s)\n",
				team_get_current_team_id(), areaID, area->name);
			return -EPERM;
		}
		// TODO: Enforce all restrictions (team, etc.)!

		oldSize = area->Size();
		if (newSize == oldSize)
			return 0;

		if (cache->type != CACHE_TYPE_RAM)
			return -EPERM;

		if (oldSize < newSize) {
			// We need to check if all areas of this cache can be resized.
			for (VMArea * other = cache->areas; other != nullptr;
					other = other->cache_next) {
				if (!other->address_space->CanResizeArea(other, newSize))
					return -EINVAL;
				anyKernelArea |= other->address_space == VMAddressSpace::Kernel();
			}
		} else {
			// We're shrinking the areas, so we must make sure the affected
			// ranges are not wired.
			for (VMArea* other = cache->areas; other != nullptr;
					other = other->cache_next) {
				anyKernelArea
					|= other->address_space == VMAddressSpace::Kernel();

				if (wait_if_area_range_is_wired(other,
						other->Base() + newSize, oldSize - newSize, &locker, &cacheLocker))
				{
					restart = true;
					break;
				}
			}
		}
	} while (restart);

	// Okay, looks good so far, so let's do it

	int priority = kernel && anyKernelArea
		? GFP_PRIO_SYSTEM : GFP_PRIO_USER;
	uint32_t allocationFlags = kernel && anyKernelArea
		? (CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL) : 0;

	if (oldSize < newSize) {
		// Growing the cache can fail, so we do it first.
		status = cache->Resize(cache->virtual_base + newSize, priority);
		if (status != 0)
			return status;
	}

	for (VMArea* other = cache->areas; other != nullptr;
			other = other->cache_next) {
		status = other->address_space->ResizeArea(other, newSize, allocationFlags);
		if (status != 0)
			break;

		// We also need to unmap all pages beyond the new size, if the area has
		// shrunk
		if (newSize < oldSize) {
			VMCacheChainLocker cacheChainLocker(cache);
			cacheChainLocker.LockAllSourceCaches();
			unmap_pages(other, other->Base() + newSize,	oldSize - newSize);
			cacheChainLocker.Unlock(cache);
		}
	}

	if (status == 0) {
		// Shrink or grow individual page protections if in use.
		if (area->page_protections != nullptr) {
			uint32_t bytes = (newSize / PAGE_SIZE + 1) / 2;
			uint8_t * newProtections = (uint8_t *)realloc_etc(area->page_protections, bytes, allocationFlags);
			if (newProtections == nullptr)
				status = -ENOMEM;
			else {
				area->page_protections = newProtections;

				if (oldSize < newSize) {
					// init the additional page protections to that of the area
					uint32_t offset = (oldSize / PAGE_SIZE + 1) / 2;
					uint32_t areaProtection = area->protection & VMA_FLAG_U_MASK;
					memset(area->page_protections + offset,
						areaProtection | (areaProtection << 4), bytes - offset);
					if ((oldSize / PAGE_SIZE) % 2 != 0) {
						uint8_t& entry = area->page_protections[offset - 1];
						entry = (entry & 0x0f) | (areaProtection << 4);
					}
				}
			}
		}
	}

	// shrinking the cache can't fail, so we do it now
	if (status == 0 && newSize < oldSize)
		status = cache->Resize(cache->virtual_base + newSize, priority);

	if (status != 0) {
		// Something failed -- resize the areas back to their original size.
		// This can fail, too, in which case we're seriously screwed.
		for (VMArea* other = cache->areas; other != nullptr;
				other = other->cache_next) {
			if (other->address_space->ResizeArea(other, oldSize, allocationFlags) != 0) {
				panic("vm_resize_area(): Failed and not being able to restore "
					"original state.");
			}
		}

		cache->Resize(cache->virtual_base + oldSize, priority);
	}

	// TODO: we must honour the lock restrictions of this area
	return status;
}

int resize_area(area_id id, size_t newSize)
{
	if(!arch_kernel_allocation_size_valid(newSize))
		return -ENOMEM;

	return vm_resize_area(id, newSize, true);
}

int __sys_resize_area(area_id id, size_t newSize)
{
	if(!arch_user_allocation_size_valid(newSize))
		return -ENOMEM;

	return vm_resize_area(id, newSize, false);
}

/*!	Transfers the specified area to a new team. The caller must be the owner
	of the area.
*/
area_id transfer_area(area_id id, void** _address, uint32_t addressSpec, pid_t target, bool kernel)
{
	area_info info;
	int status = get_area_info(id, &info);
	if (status != 0)
		return status;

	if (info.team != getpid())
		return -EPERM;

	area_id clonedArea = vm_clone_area(target, info.name, (vaddr_t *)_address,
			addressSpec, info.protection, false, id, kernel);

	if (clonedArea < 0)
		return clonedArea;

	status = vm_delete_area(info.team, id, kernel);

	if (status != 0) {
		vm_delete_area(target, clonedArea, kernel);
		return status;
	}

	return clonedArea;
}

int __sys_transfer_area(int area, void** userAddress, uint32_t addressSpec, pid_t target)
{
	// filter out some unavailable values (for userland)
	switch (addressSpec) {
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
			return -EINVAL;
	}

	void* address = nullptr;
	if (!userAddress
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	area_id newArea = transfer_area(area, &address, addressSpec, target, false);
	if (newArea < 0)
		return newArea;

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0) {
		delete_area(newArea);
		return -EFAULT;
	}

	return newArea;
}

area_id clone_area(const char* name, void** _address, uint32_t addressSpec,
	uint32_t protection, area_id source)
{
	if ((protection & VMA_FLAG_S_MASK) == 0)
		protection |= VMA_FLAG_S_READ | VMA_FLAG_S_WRITE;

	return vm_clone_area(VMAddressSpace::KernelID(), name, (vaddr_t*) _address,
			addressSpec, protection, false, source, true);
}


int __sys_clone_area(const char* userName, void** userAddress, uint32_t addressSpec,
	uint32_t protection, area_id sourceArea)
{
	char name[B_OS_NAME_LENGTH];
	void* address = nullptr;

	// filter out some unavailable values (for userland)
	switch (addressSpec) {
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
			return -EINVAL;
	}
	if ((protection & ~B_USER_AREA_FLAGS) != 0)
		return -EINVAL;

	if (!is_userspace_ptr_valid(userName, 1)
		|| !is_userspace_ptr_valid(userAddress, current_team_size_of_pointer())
		|| strlcpy_from_user(name, userName, sizeof(name)) < 0
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	fix_protection(&protection);

	area_id clonedArea = vm_clone_area(VMAddressSpace::CurrentID(), name,
		(vaddr_t *)&address, addressSpec, protection, false, sourceArea,
		false);
	if (clonedArea < 0)
		return clonedArea;

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0) {
		delete_area(clonedArea);
		return -EFAULT;
	}

	return clonedArea;
}

int set_area_protection(area_id area, uint32_t newProtection)
{
	return vm_set_area_protection(VMAddressSpace::KernelID(), area, newProtection, true);
}

int __sys_set_area_protection(area_id area, uint32_t newProtection)
{
	if ((newProtection & ~B_USER_PROTECTION) != 0)
		return -EINVAL;
	return vm_set_area_protection(VMAddressSpace::CurrentID(), area, newProtection, false);
}

int create_area(const char* userName, void** userAddress, uint32_t addressSpec,
		size_t size, uint32_t lock, uint32_t protection)
{
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = addressSpec;
	virtualRestrictions.address = *userAddress;

	physical_address_restrictions physicalRestrictions = {};

	if (!arch_kernel_allocation_size_valid(size)) {
		// Cap invalid size
		return -EINVAL;
	}

	return vm_create_anonymous_area_etc(VMAddressSpace::KernelID(), userName, size, lock, protection, 0, 0,
			&virtualRestrictions, &physicalRestrictions, true, (vaddr_t *)userAddress);
}

int reserve_address_range(uintptr_t * userAddress, uint32_t addressSpec, size_t size)
{
	if (!arch_kernel_allocation_size_valid(size))
		return -EINVAL;

	return vm_reserve_address_range(B_SYSTEM_TEAM, userAddress, addressSpec, size, RESERVED_AVOID_BASE, 0);
}

int unreserve_address_range(uintptr_t userAddress, size_t size)
{
	if (!arch_kernel_allocation_size_valid(size))
		return -EINVAL;

	return vm_unreserve_address_range(B_SYSTEM_TEAM, userAddress, size);
}

int map_physical_memory_pfns(const char * name, void ** address,
			uint32_t addressSpec, size_t numberOfPages, uint32_t protection,
			vm_pindex_t * physicalAddressIndexes)
{
	return vm_map_physical_memory_pfns(B_SYSTEM_TEAM, name, (vaddr_t *) address,
			addressSpec, numberOfPages, protection, physicalAddressIndexes);
}

int map_physical_memory(const char * name, void ** address,
		uint32_t addressSpec, size_t size, uint32_t protection,
		vm_paddr_t physicalAddress)
{
	if (!arch_kernel_allocation_size_valid(size))
		return -ENOMEM;

	return vm_map_physical_memory(B_SYSTEM_TEAM, name, (vaddr_t *)address,
			addressSpec, size, protection, physicalAddress, false);
}

int __sys_create_area(const char* userName, void** userAddress, uint32_t addressSpec,
	size_t size, uint32_t lock, uint32_t protection)
{
	char name[B_OS_NAME_LENGTH];
	void* address = nullptr;

	// filter out some unavailable values (for userland)
	switch (addressSpec) {
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
			return -EINVAL;
	}

	if ((protection & ~(B_USER_AREA_FLAGS | B_SHARED_AREA | B_COPYONCE_AREA | B_NOCOPY_AREA)) != 0)
		return -EINVAL;

	if (!is_userspace_ptr_valid(userName, 1)
		|| !is_userspace_ptr_valid(userAddress, current_team_size_of_pointer())
		|| strlcpy_from_user(name, userName, B_OS_NAME_LENGTH) < 0
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	if (addressSpec == B_EXACT_ADDRESS && is_kernelspace_ptr_valid(address, 1))
		return -EINVAL;

	if (!arch_user_allocation_size_valid(size))
		return -ENOMEM;

	if (addressSpec == B_ANY_ADDRESS)
		addressSpec = B_RANDOMIZED_ANY_ADDRESS;
	if (addressSpec == B_BASE_ADDRESS)
		addressSpec = B_RANDOMIZED_BASE_ADDRESS;

	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address = address;
	virtualRestrictions.address_specification = addressSpec;

	fix_protection(&protection);

	physical_address_restrictions physicalRestrictions = {};

	int area = vm_create_anonymous_area_etc(VMAddressSpace::CurrentID(), name, size, lock,
			protection, 0, 0, &virtualRestrictions,
		&physicalRestrictions, false, (vaddr_t *)&address);

	if (area >= 0
		&& memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0)
	{
		delete_area(area);
		return -EFAULT;
	}

	return area;
}

int __sys_map_file(const char* userName, void** userAddress, uint32_t addressSpec,
		size_t size, uint32_t protection, bool privateMap, bool unmapAddressRange,
		int fd, off_t offset)
{
	char name[B_OS_NAME_LENGTH];
	void* address = nullptr;
	int area;

	if ((protection & ~B_USER_AREA_FLAGS) != 0)
		return -EINVAL;

	if (!is_userspace_ptr_valid(userName, 1)
		|| !is_userspace_ptr_valid(userAddress, current_team_size_of_pointer())
		|| strlcpy_from_user(name, userName, B_OS_NAME_LENGTH) < 0
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	if (addressSpec == B_EXACT_ADDRESS) {
		if ((uintptr_t)address + size < (uintptr_t)address
				|| (uintptr_t)address % PAGE_SIZE != 0) {
			return -EINVAL;
		}
		if (!is_userspace_ptr_valid((void *)address, size)) {
			if((uintptr_t)address >= PAGE_SIZE && (uintptr_t)address <= team_get_current_team()->addressSpace->EndAddress()) {
				return -ENOMEM;
			}
			return -EFAULT;
		}
	}

	if (!arch_user_allocation_size_valid(size))
		return -ENOMEM;

	fix_protection(&protection);

	area = _vm_map_file(VMAddressSpace::CurrentID(), name, (vaddr_t *)&address,
		addressSpec, size, protection, privateMap, unmapAddressRange, fd, offset,
		false);

	if (area < 0)
		return area;

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0) {
		vm_delete_area(VMAddressSpace::CurrentID(), area, false);
		return -EFAULT;
	}

	return area;
}

int vm_unreserve_address_range(pid_t team, vaddr_t address, size_t size) {
	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return -ESRCH;
	VMAddressSpace * addressSpace = locker.AddressSpace();

	return addressSpace->UnreserveAddressRange(address, size,
			addressSpace == VMAddressSpace::Kernel() ? (CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL) : 0);
}

int vm_reserve_address_range(pid_t team, vaddr_t * _address,
		uint32_t addressSpec, size_t size, uint32_t flags, size_t alignment)
{
	if (size == 0)
		return -EINVAL;

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return -ESRCH;

	virtual_address_restrictions addressRestrictions = { };
	addressRestrictions.address = (void *) (*_address);
	addressRestrictions.address_specification = addressSpec;
	addressRestrictions.alignment = alignment;
	VMAddressSpace * addressSpace = locker.AddressSpace();
	return addressSpace->ReserveAddressRange(size, &addressRestrictions, flags,
			addressSpace == VMAddressSpace::Kernel() ? (CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL) : 0,
			(void **)_address);
}

int __sys_reserve_address_range(uintptr_t * userAddress, uint32_t addressSpec, size_t size, size_t alignment)
{
	// filter out some unavailable values (for userland)
	switch (addressSpec) {
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
			return -EINVAL;
	}

	vaddr_t address = 0;

	if (memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) != 0)
		return -EFAULT;

	if (!arch_user_allocation_size_valid(size))
		return -ENOMEM;

	int status = vm_reserve_address_range(VMAddressSpace::CurrentID(), &address, addressSpec, size, RESERVED_AVOID_BASE, alignment);

	if (status != 0)
		return status;

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) != 0) {
		vm_unreserve_address_range(VMAddressSpace::CurrentID(), address, size);
		return -EFAULT;
	}

	return 0;
}

int __sys_unreserve_address_range(uintptr_t userAddress, size_t size)
{
	if (!arch_user_allocation_size_valid(size))
		return -EINVAL;

	return vm_unreserve_address_range(VMAddressSpace::CurrentID(), userAddress, size);
}

void vm_remove_all_page_mappings(struct page * page)
{
	while (vm_page_mapping* mapping = page->mappings.Head()) {
		VMArea* area = mapping->area;
		VMTranslationMap* map = area->address_space->TranslationMap();
		uintptr_t address = virtual_page_address(area, page);
		map->UnmapPage(area, address, false);
	}
}

/*!	The page's cache must be locked.
*/
bool vm_test_map_modification(struct page* page)
{
	if (page->modified)
		return true;

	vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
	vm_page_mapping* mapping;
	while ((mapping = iterator.Next()) != nullptr) {
		VMArea * area = mapping->area;
		VMTranslationMap* map = area->address_space->TranslationMap();

		vm_paddr_t physicalAddress;
		uint32_t flags;

		map->Lock();
		map->Query(virtual_page_address(area, page), &physicalAddress, &flags);
		map->Unlock();

		if ((flags & PAGE_MODIFIED) != 0)
			return true;
	}

	return false;
}

void vm_clear_map_flags(struct page * page, uint32_t flags)
{
	if ((flags & PAGE_ACCESSED) != 0)
		page->accessed = false;
	if ((flags & PAGE_MODIFIED) != 0)
		page->modified = false;

	vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
	vm_page_mapping* mapping;
	while ((mapping = iterator.Next()) != nullptr) {
		VMArea * area = mapping->area;
		VMTranslationMap* map = area->address_space->TranslationMap();

		map->Lock();
		map->ClearFlags(virtual_page_address(area, page), flags);
		map->Unlock();
	}
}

int32_t vm_clear_page_mapping_accessed_flags(struct page *page)
{
	int32_t count = 0;

	vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
	vm_page_mapping* mapping;
	while ((mapping = iterator.Next()) != nullptr) {
		VMArea * area = mapping->area;
		VMTranslationMap* map = area->address_space->TranslationMap();

		bool modified;
		if (map->ClearAccessedAndModified(area,
				virtual_page_address(area, page), false, modified)) {
			count++;
		}

		page->modified |= modified;
	}

	if (page->accessed) {
		count++;
		page->accessed = false;
	}

	return count;
}

int32_t vm_remove_all_page_mappings_if_unaccessed(struct page *page)
{
	ASSERT(page->wired_count == 0);

	if (page->accessed)
		return vm_clear_page_mapping_accessed_flags(page);

	while (vm_page_mapping* mapping = page->mappings.Head()) {
		VMArea * area = mapping->area;
		VMTranslationMap* map = area->address_space->TranslationMap();
		uintptr_t address = virtual_page_address(area, page);
		bool modified = false;

		if (map->ClearAccessedAndModified(area, address, true, modified)) {
			page->accessed = true;
			page->modified |= modified;
			return vm_clear_page_mapping_accessed_flags(page);
		}

		page->modified |= modified;
	}

	return 0;
}

int __sys_map_physical_memory(const char *userName, void **userAddress,
		uint32_t addressSpec, size_t size, uint32_t protection,
		vm_paddr_t physicalAddress)
{
	char name[B_OS_NAME_LENGTH];
	vaddr_t address = 0;
	int area;

	if (!(team_get_current_team()->permission & TASK_CAPABILITY_DEVICE_DRIVER))
		return -EPERM;

	if ((protection & ~(B_USER_AREA_FLAGS | B_MTR_CACHE_MASK | B_DEVICE_AREA)) != 0)
		return -EINVAL;

	// Disallow mapping of secure kernel managed memory to user space
	if(!(team_get_current_team()->permission & TASK_CAPABILITY_TRUSTED_CONTROL)) {
		for(uint32_t i = 0 ; i < gKernelArgs->all_memory.num ; ++i) {
			if(std::max<uint64_t>(physicalAddress, gKernelArgs->all_memory.ranges[i].start) <
			   std::min<uint64_t>(physicalAddress + size,
					   gKernelArgs->all_memory.ranges[i].start +
					   gKernelArgs->all_memory.ranges[i].size))
			{
				return -EPERM;
			}
		}
	}

	if (!is_userspace_ptr_valid(userName, 1)
		|| !is_userspace_ptr_valid(userAddress, current_team_size_of_pointer())
		|| strlcpy_from_user(name, userName, B_OS_NAME_LENGTH) < 0
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	if (addressSpec == B_EXACT_ADDRESS) {
		if ((uintptr_t)address + size < (uintptr_t)address
				|| (uintptr_t)address % PAGE_SIZE != 0) {
			return -EINVAL;
		}
		if (!is_userspace_ptr_valid((void *)address, size)) {
			return -EFAULT;
		}
	}

	if (!arch_user_allocation_size_valid(size))
		return -ENOMEM;

	fix_protection(&protection);

	area = vm_map_physical_memory(VMAddressSpace::CurrentID(), name, &address, addressSpec, size,
			protection, physicalAddress, false);

	if(area < 0) {
		return area;
	}

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0) {
		vm_delete_area(VMAddressSpace::CurrentID(), area, false);
		return -EFAULT;
	}

	return area;
}

int __sys_map_physical_memory_pfns(const char *userName, void **userAddress,
		uint32_t addressSpec, size_t numberOfPages, uint32_t protection,
		vm_pindex_t *userPhysicalIndexes)
{
	char name[B_OS_NAME_LENGTH];
	void* address = nullptr;
	int area;
	size_t size = numberOfPages << PAGE_SHIFT;

	if (!(team_get_current_team()->permission & TASK_CAPABILITY_DEVICE_DRIVER))
		return -EPERM;

	if ((protection & ~(B_USER_AREA_FLAGS | B_MTR_CACHE_MASK | B_DEVICE_AREA)) != 0)
		return -EINVAL;

	if (!is_userspace_ptr_valid(userName, 1)
		|| !is_userspace_ptr_valid(userAddress, current_team_size_of_pointer())
		|| strlcpy_from_user(name, userName, B_OS_NAME_LENGTH) < 0
		|| memcpy_from_user(&address, userAddress, current_team_size_of_pointer()) < 0)
		return -EFAULT;

	if (addressSpec == B_EXACT_ADDRESS) {
		if ((uintptr_t)address + size < (uintptr_t)address
				|| (uintptr_t)address % PAGE_SIZE != 0) {
			return -EINVAL;
		}
		if (!is_userspace_ptr_valid((void *)address, size)) {
			return -EFAULT;
		}
	}

	if (!arch_user_allocation_size_valid(size) || size < numberOfPages)
		return -ENOMEM;

	vm_pindex_t * indexes = new(std::nothrow) vm_pindex_t[numberOfPages];

	if(!indexes) {
		return -ENOMEM;
	}

	ArrayDeleter<vm_pindex_t> indexesDeleter(indexes);

	if(memcpy_from_user(indexes, userPhysicalIndexes, sizeof(vm_pindex_t) * numberOfPages) < 0) {
		return -ENOMEM;
	}

	// Disallow mapping of secure kernel managed memory to user space
	if(!(team_get_current_team()->permission & TASK_CAPABILITY_TRUSTED_CONTROL)) {
		for(size_t j = 0 ; j < numberOfPages ; ++j) {
			vm_paddr_t physicalAddress = ((vm_paddr_t)indexes[j]) << PAGE_SHIFT;
			for(uint32_t i = 0 ; i < gKernelArgs->all_memory.num ; ++i) {
				if(physicalAddress >= gKernelArgs->all_memory.ranges[i].start &&
						physicalAddress < (gKernelArgs->all_memory.ranges[i].start +
								gKernelArgs->all_memory.ranges[i].size))
				{
					return -EPERM;
				}
			}
		}
	}

	fix_protection(&protection);

	area = vm_map_physical_memory_pfns(VMAddressSpace::CurrentID(), name, (vaddr_t *)&address, addressSpec,
			numberOfPages, protection, indexes);

	if(area < 0) {
		return area;
	}

	if (memcpy_to_user(userAddress, &address, current_team_size_of_pointer()) < 0) {
		vm_delete_area(VMAddressSpace::CurrentID(), area, false);
		return -EFAULT;
	}

	return area;
}

int __sys_cache_control(uintptr_t base, size_t size, vm_paddr_t physAddr,
		int mode) {
	struct task * task = team_get_current_team();
	if (!is_userspace_ptr_valid((const void *)base, size))
		return -EFAULT;
	if (!(task->permission & TASK_CAPABILITY_DEVICE_DRIVER))
		return -EPERM;
	if(!(task->permission & TASK_CAPABILITY_TRUSTED_CONTROL)) {
		for(uint32_t i = 0 ; i < gKernelArgs->all_memory.num ; ++i) {
			if(std::max<uint64_t>(physAddr, gKernelArgs->all_memory.ranges[i].start) <
			   std::min<uint64_t>(physAddr + size,
					   gKernelArgs->all_memory.ranges[i].start +
					   gKernelArgs->all_memory.ranges[i].size))
			{
				return -EPERM;
			}
		}
	}
	return arch_vm_cache_control(base, size, physAddr, mode);
}

int __sys_set_area_permission(area_id id, mode_t mode, uid_t uid, gid_t gid)
{
	// lock the address space and make sure the area isn't wired
	AddressSpaceWriteLocker locker;
	VMArea* area;
	AreaCacheLocker cacheLocker;

	do {
		int status = locker.SetFromArea(VMAddressSpace::CurrentID(), id, area);
		if (status != 0)
			return status;
		cacheLocker.SetTo(area);
	} while (wait_if_area_is_wired(area, &locker, &cacheLocker));

	cacheLocker.Unlock();

	area->mode = mode;
	area->owner_gid = gid;
	area->owner_uid = uid;

	return 0;
}

vm_paddr_t vm_allocate_early_physical_page(void)
{
	vm_paddr_t phys;
	if(boot_allocate_physical_memory(PAGE_SIZE, PAGE_SIZE, &phys) < 0)
		return 0;
	return phys;
}

uintptr_t vm_allocate_early(size_t virtualSize, size_t physicalSize, uint32_t attributes, uintptr_t alignment)
{
	if (physicalSize > virtualSize)
		physicalSize = virtualSize;

	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;

	ASSERT(powerof2(alignment));

	uintptr_t virtualBase = boot_allocate_virtual_memory(virtualSize,
			alignment);

	//dprintf("vm_allocate_early: vaddr 0x%lx\n", virtualBase);
	if (virtualBase == 0) {
		panic("vm_allocate_early: could not allocate virtual address\n");
	}

	ASSERT(!(virtualBase & (alignment - 1)));

	// map the pages
	for (uint32_t i = 0; i < ROUND_PGUP(physicalSize) / PAGE_SIZE; i++) {
		vm_paddr_t physicalAddress = vm_allocate_early_physical_page();
		if (physicalAddress == 0)
			panic("error allocating early page!\n");

		arch_vm_translation_map_early_map(virtualBase + i * PAGE_SIZE,
			physicalAddress, attributes,
			&vm_allocate_early_physical_page);
	}

	return virtualBase;
}

int vm_get_page_mapping(pid_t team, uintptr_t vaddr, vm_paddr_t * paddr)
{
	VMAddressSpace * addressSpace = VMAddressSpace::Get(team);
	if (addressSpace == nullptr)
		return -ESRCH;

	VMTranslationMap* map = addressSpace->TranslationMap();

	map->Lock();
	uint32_t dummyFlags;
	int status = map->Query(vaddr, paddr, &dummyFlags);
	map->Unlock();

	addressSpace->Put();
	return status;
}

static int
discard_area_range(VMArea* area, uintptr_t address, size_t size)
{
	uintptr_t offset;
	if (!intersect_area(area, address, size, offset))
		return 0;

	// If someone else uses the area's cache or it's not an anonymous cache, we
	// can't discard.
	VMCache* cache = vm_area_get_locked_cache(area);
	if (cache->areas != area || area->cache_next != NULL
		|| !cache->consumers.IsEmpty() || cache->type != CACHE_TYPE_RAM) {
		vm_area_put_locked_cache(cache);
		return 0;
	}

	VMCacheChainLocker cacheChainLocker(cache);
	cacheChainLocker.LockAllSourceCaches();

	unmap_pages(area, address, size);

	// Since VMCache::Discard() can temporarily drop the lock, we must
	// unlock all lower caches to prevent locking order inversion.
	cacheChainLocker.Unlock(cache);
	cache->Discard(cache->virtual_base + offset, size);
	cache->ReleaseRefAndUnlock();

	return 0;
}

static int
discard_address_range(VMAddressSpace* addressSpace, uintptr_t address, size_t size,
	bool)
{
	for (VMAddressSpace::AreaRangeIterator it
		= addressSpace->GetAreaRangeIterator(address, size);
			VMArea* area = it.Next();) {
		int error = discard_area_range(area, address, size);
		if (error != 0)
			return error;
	}

	return 0;
}


int
__sys_sync_memory(void* _address, size_t size, uint32_t flags)
{
	uintptr_t address = (uintptr_t)_address;
	size = round_page(size);

	// check params
	if ((address % PAGE_SIZE) != 0)
		return -EINVAL;

	if (!is_userspace_ptr_valid(_address, size)) {
		// weird error code required by POSIX
		return -ENOMEM;
	}

	bool writeSync = (flags & MS_SYNC) != 0;
	bool writeAsync = (flags & MS_ASYNC) != 0;
	if (writeSync && writeAsync)
		return -EINVAL;

	if (size == 0 || (!writeSync && !writeAsync))
		return 0;

	// iterate through the range and sync all concerned areas
	while (size > 0) {
		// read lock the address space
		AddressSpaceReadLocker locker;
		int error = locker.SetTo(team_get_current_team_id());
		if (error != 0)
			return error;

		// get the first area
		VMArea* area = locker.AddressSpace()->LookupArea(address);
		if (area == NULL)
			return -ENOMEM;

		size_t offset = address - area->Base();
		size_t rangeSize = std::min(area->Size() - offset, size);
		offset += area->cache_offset;

		// lock the cache
		AreaCacheLocker cacheLocker(area);
		if (!cacheLocker)
			return -EINVAL;
		VMCache* cache = area->cache;

		locker.Unlock();

		size_t firstPage = offset >> PAGE_SHIFT;
		size_t endPage = firstPage + (rangeSize >> PAGE_SHIFT);

		// write the pages
		if (cache->type == CACHE_TYPE_VNODE) {
			if (writeSync) {
				// synchronous
				error = vm_page_write_modified_page_range(cache, firstPage,
					endPage);
				if (error != 0)
					return error;
			} else {
				// asynchronous
				vm_page_schedule_write_page_range(cache, firstPage, endPage);
				// TODO: This is probably not quite what is supposed to happen.
				// Especially when a lot has to be written, it might take ages
				// until it really hits the disk.
			}
		}

		address += rangeSize;
		size -= rangeSize;
	}

	// NOTE: If I understand it correctly the purpose of MS_INVALIDATE is to
	// synchronize multiple mappings of the same file. In our VM they never get
	// out of sync, though, so we don't have to do anything.

	return 0;
}

int
__sys_memory_advice(void* _address, size_t size, uint32_t advice)
{
	uintptr_t address = (uintptr_t) _address;
	if ((address % PAGE_SIZE) != 0)
		return -EINVAL;

	size = round_page(size);

	if (!is_userspace_ptr_valid(_address, size)) {
		// weird error code required by POSIX
		return -ENOMEM;
	}

	switch (advice) {
		case MADV_NORMAL:
		case MADV_SEQUENTIAL:
		case MADV_RANDOM:
		case MADV_WILLNEED:
		case MADV_DONTNEED:
			// TODO: Implement!
			break;

		case MADV_FREE:
		{
			AddressSpaceWriteLocker locker;
			do {
				int status = locker.SetTo(getpid());
				if (status != 0)
					return status;
			} while (wait_if_address_range_is_wired(locker.AddressSpace(),
					address, size, &locker));

			discard_address_range(locker.AddressSpace(), address, size, false);
			break;
		}

		default:
			return -EINVAL;
	}

	return 0;
}


#if defined(CONFIG_KDEBUGGER)
const char* vm_cache_type_to_string(int32_t type);

static void
dump_area_struct(VMArea* area, bool mappings)
{
	kprintf("AREA: %p\n", area);
	kprintf("name:\t\t'%s'\n", area->name);
	kprintf("owner:\t\t0x%" PRIx32 "\n", area->address_space->ID());
	kprintf("id:\t\t0x%" PRIx32 "\n", area->id);
	kprintf("base:\t\t0x%" PRIxPTR "\n", area->Base());
	kprintf("size:\t\t0x%" PRIxPTR "\n", area->Size());
	kprintf("protection:\t0x%" PRIx32 "\n", area->protection & ~B_KERNEL_MEMORY_TYPE_MASK);
	kprintf("wiring:\t\t0x%x\n", area->wiring);
	kprintf("memory_type:\t%#" PRIx32 "\n", area->protection & B_KERNEL_MEMORY_TYPE_MASK);
	kprintf("cache:\t\t%p\n", area->cache);
	kprintf("cache_type:\t%s\n", vm_cache_type_to_string(area->cache_type));
	kprintf("cache_offset:\t0x%zx\n", (size_t)area->cache_offset);
	kprintf("cache_next:\t%p\n", area->cache_next);
	kprintf("cache_prev:\t%p\n", area->cache_prev);

	auto iterator = area->mappings.GetIterator();
	if (mappings) {
		kprintf("page mappings:\n");
		while (iterator.HasNext()) {
			auto * mapping = iterator.Next();
			kprintf("  %p", mapping->page);
		}
		kprintf("\n");
	} else {
		uint32_t count = 0;
		while (iterator.Next() != nullptr) {
			count++;
		}
		kprintf("page mappings:\t%" PRIu32 "\n", count);
	}
}


static int
dump_area(int argc, char** argv)
{
	bool mappings = false;
	bool found = false;
	int32_t index = 1;
	struct VMArea * area;
	uintptr_t num;

	if (argc < 2 || !strcmp(argv[1], "--help")) {
		kprintf("usage: area [-m] [id|contains|address|name] <id|address|name>\n"
			"All areas matching either id/address/name are listed. You can\n"
			"force to check only a specific item by prefixing the specifier\n"
			"with the id/contains/address/name keywords.\n"
			"-m shows the area's mappings as well.\n");
		return 0;
	}

	if (!strcmp(argv[1], "-m")) {
		mappings = true;
		index++;
	}

	int32_t mode = 0xf;
	if (!strcmp(argv[index], "id"))
		mode = 1;
	else if (!strcmp(argv[index], "contains"))
		mode = 2;
	else if (!strcmp(argv[index], "name"))
		mode = 4;
	else if (!strcmp(argv[index], "address"))
		mode = 0;
	if (mode != 0xf)
		index++;

	if (index >= argc) {
		kprintf("No area specifier given.\n");
		return 0;
	}

	num = parse_expression(argv[index]);

	if (mode == 0) {
		dump_area_struct((struct VMArea *)num, mappings);
	} else {
		// walk through the area list, looking for the arguments as a name

		auto it = VMAreaHash::GetIterator();
		while ((area = it.Next()) != nullptr) {
			if (((mode & 4) != 0 && area->name[0] != '\0'
					&& !strcmp(argv[index], area->name))
				|| (num != 0 && (((mode & 1) != 0 && (uintptr_t)area->id == num)
					|| (((mode & 2) != 0 && area->Base() <= num
						&& area->Base() + area->Size() > num))))) {
				dump_area_struct(area, mappings);
				found = true;
			}
		}

		if (!found)
			kprintf("could not find area %s (%" PRIxPTR ")\n", argv[index], num);
	}

	return 0;
}


static int
dump_area_list(int argc, char** argv)
{
	struct VMArea * area;
	const char* name = nullptr;
	int32_t id = 0;

	if (argc > 1) {
		id = parse_expression(argv[1]);
		if (id == 0)
			name = argv[1];
	}

	kprintf("%-*s      id  %-*s    %-*sprotect lock  name\n",
		B_PRINTF_POINTER_WIDTH, "addr", B_PRINTF_POINTER_WIDTH, "base",
		B_PRINTF_POINTER_WIDTH, "size");

	auto it = VMAreaHash::GetIterator();
	while ((area = it.Next()) != nullptr) {
		if ((id != 0 && area->address_space->ID() != id)
			|| (name != nullptr && strstr(area->name, name) == nullptr))
			continue;

		kprintf("%p %5" PRIx32 "  %p  %p %4" PRIx32 " %4d  %s\n", area,
			area->id, (void*)area->Base(), (void*)area->Size(),
			area->protection, area->wiring, area->name);
	}
	return 0;
}


static int
dump_available_memory(int, char**)
{
	kprintf("Available memory: %" B_PRIdOFF "/%zu bytes\n",
		sAvailableMemory, (size_t)vm_page_num_pages() * PAGE_SIZE);
	return 0;
}

static int
dump_mapping_info(int argc, char** argv)
{
	bool reverseLookup = false;
	bool pageLookup = false;

	int argi = 1;
	for (; argi < argc && argv[argi][0] == '-'; argi++) {
		const char* arg = argv[argi];
		if (strcmp(arg, "-r") == 0) {
			reverseLookup = true;
		} else if (strcmp(arg, "-p") == 0) {
			reverseLookup = true;
			pageLookup = true;
		} else {
			print_debugger_command_usage(argv[0]);
			return 0;
		}
	}

	// We need at least one argument, the address. Optionally a thread ID can be
	// specified.
	if (argi >= argc || argi + 2 < argc) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	uint64_t addressValue;
	if (!evaluate_debug_expression(argv[argi++], &addressValue, false))
		return 0;

	struct task* team = nullptr;
	if (argi < argc) {
		uint64_t threadID;
		if (!evaluate_debug_expression(argv[argi++], &threadID, false))
			return 0;

		struct thread* thread = thread::GetDebug(threadID);
		if (thread == nullptr) {
			kprintf("Invalid thread/team ID \"%s\"\n", argv[argi - 1]);
			return 0;
		}

		team = thread->task;
	}

	if (reverseLookup) {
		vm_paddr_t physicalAddress;
		if (pageLookup) {
			struct page* page = (struct page*)(uintptr_t)addressValue;
			physicalAddress = page->pfn * PAGE_SIZE;
		} else {
			physicalAddress = (vm_paddr_t)addressValue;
			physicalAddress -= physicalAddress % PAGE_SIZE;
		}

		kprintf("    Team     Virtual Address      Area\n");
		kprintf("--------------------------------------\n");

		struct Callback : VMTranslationMap::ReverseMappingInfoCallback {
			Callback()
				:
				fAddressSpace(nullptr)
			{
			}

			void SetAddressSpace(VMAddressSpace* addressSpace)
			{
				fAddressSpace = addressSpace;
			}

			bool HandleVirtualAddress(uintptr_t virtualAddress) override
			{
				kprintf("%8" PRId32 "  %#18" PRIxPTR, fAddressSpace->ID(),
					virtualAddress);
				if (VMArea * area = fAddressSpace->LookupArea(virtualAddress))
					kprintf("  %8" PRId32 " %s\n", area->id, area->name);
				else
					kprintf("\n");
				return false;
			}

		private:
			VMAddressSpace *	fAddressSpace;
		} callback;

		if (team != nullptr) {
			// team specified -- get its address space
			VMAddressSpace* addressSpace = team->addressSpace;
			if (addressSpace == nullptr) {
				kprintf("Failed to get address space!\n");
				return 0;
			}

			callback.SetAddressSpace(addressSpace);
			addressSpace->TranslationMap()->DebugGetReverseMappingInfo(physicalAddress, callback);
		} else {
			// no team specified -- iterate through all address spaces
			for (VMAddressSpace* addressSpace = VMAddressSpace::DebugFirst();
				addressSpace != nullptr;
				addressSpace = VMAddressSpace::DebugNext(addressSpace)) {
				callback.SetAddressSpace(addressSpace);
				addressSpace->TranslationMap()->DebugGetReverseMappingInfo(physicalAddress, callback);
			}
		}
	} else {
		// get the address space
		uintptr_t virtualAddress = (uintptr_t)addressValue;
		virtualAddress -= virtualAddress % PAGE_SIZE;
		VMAddressSpace* addressSpace;
		if (virtualAddress >= VM_KERNEL_SPACE_BASE) {
			addressSpace = VMAddressSpace::Kernel();
		} else if (team != nullptr) {
			addressSpace = team->addressSpace;
		} else {
			struct thread* thread = debug_get_debugged_thread();
			if (thread == nullptr || thread->task == nullptr) {
				kprintf("Failed to get team!\n");
				return 0;
			}

			addressSpace = thread->task->addressSpace;
		}

		if (addressSpace == nullptr) {
			kprintf("Failed to get address space!\n");
			return 0;
		}

		// let the translation map implementation do the job
		addressSpace->TranslationMap()->DebugPrintMappingInfo(virtualAddress);
	}

	return 0;
}


static int
display_mem(int argc, char** argv)
{
	bool physical = false;
	uintptr_t copyAddress;
	int32_t displayWidth;
	int32_t itemSize;
	int32_t num = -1;
	uintptr_t address;
	int i = 1, j;

	if (argc > 1 && argv[1][0] == '-') {
		if (!strcmp(argv[1], "-p") || !strcmp(argv[1], "--physical")) {
			physical = true;
			i++;
		} else
			i = 99;
	}

	if (argc < i + 1 || argc > i + 2) {
		kprintf("usage: dl/dw/ds/db/string [-p|--physical] <address> [num]\n"
			"\tdl - 8 bytes\n"
			"\tdw - 4 bytes\n"
			"\tds - 2 bytes\n"
			"\tdb - 1 byte\n"
			"\tstring - a whole string\n"
			"  -p or --physical only allows memory from a single page to be "
			"displayed.\n");
		return 0;
	}

	address = parse_expression(argv[i]);

	if (argc > i + 1)
		num = parse_expression(argv[i + 1]);

	// build the format string
	if (strcmp(argv[0], "db") == 0) {
		itemSize = 1;
		displayWidth = 16;
	} else if (strcmp(argv[0], "ds") == 0) {
		itemSize = 2;
		displayWidth = 8;
	} else if (strcmp(argv[0], "dw") == 0) {
		itemSize = 4;
		displayWidth = 4;
	} else if (strcmp(argv[0], "dl") == 0) {
		itemSize = 8;
		displayWidth = 2;
	} else if (strcmp(argv[0], "string") == 0) {
		itemSize = 1;
		displayWidth = -1;
	} else {
		kprintf("display_mem called in an invalid way!\n");
		return 0;
	}

	if (num <= 0)
		num = displayWidth;

	void* physicalPageHandle = nullptr;

	if (physical) {
		int32_t offset = address & (PAGE_SIZE - 1);
		if (num * itemSize + offset > PAGE_SIZE) {
			num = (PAGE_SIZE - offset) / itemSize;
			kprintf("NOTE: number of bytes has been cut to page size\n");
		}

		address = rounddown2(address, PAGE_SIZE);

		if (vm_get_physical_page_debug(address, &copyAddress,
				&physicalPageHandle) != 0) {
			kprintf("getting the hardware page failed.");
			return 0;
		}

		address += offset;
		copyAddress += offset;
	} else
		copyAddress = address;

	if (!strcmp(argv[0], "string")) {
		kprintf("%p \"", (char*)copyAddress);

		// string mode
		for (i = 0; true; i++) {
			char c;
			if (debug_memcpy(B_CURRENT_TEAM, &c, (char*)copyAddress + i, 1)
					!= 0
				|| c == '\0') {
				break;
			}

			if (c == '\n')
				kprintf("\\n");
			else if (c == '\t')
				kprintf("\\t");
			else {
				if (!isprint(c))
					c = '.';

				kprintf("%c", c);
			}
		}

		kprintf("\"\n");
	} else {
		// number mode
		for (i = 0; i < num; i++) {
			uint64_t value;

			if ((i % displayWidth) == 0) {
				int32_t displayed = MIN(displayWidth, (num-i)) * itemSize;
				if (i != 0)
					kprintf("\n");

				kprintf("[0x%" PRIxPTR "]  ", address + i * itemSize);

				for (j = 0; j < displayed; j++) {
					char c;
					if (debug_memcpy(B_CURRENT_TEAM, &c,
							(char*)copyAddress + i * itemSize + j, 1) != 0) {
						displayed = j;
						break;
					}
					if (!isprint(c))
						c = '.';

					kprintf("%c", c);
				}
				if (num > displayWidth) {
					// make sure the spacing in the last line is correct
					for (j = displayed; j < displayWidth * itemSize; j++)
						kprintf(" ");
				}
				kprintf("  ");
			}

			if (debug_memcpy(B_CURRENT_TEAM, &value,
					(uint8_t*)copyAddress + i * itemSize, itemSize) != 0) {
				kprintf("read fault");
				break;
			}

			switch (itemSize) {
				case 1:
					kprintf(" %02" PRIx8, *(uint8_t*)&value);
					break;
				case 2:
					kprintf(" %04" PRIx16, *(uint16_t*)&value);
					break;
				case 4:
					kprintf(" %08" PRIx32, *(uint32_t*)&value);
					break;
				case 8:
					kprintf(" %016" PRIx64, *(uint64_t*)&value);
					break;
			}
		}

		kprintf("\n");
	}

	if (physical) {
		copyAddress = rounddown2(copyAddress, PAGE_SIZE);
		vm_put_physical_page_debug(copyAddress, physicalPageHandle);
	}
	return 0;
}

static int dump_cache(int argc, char** argv)
{
	VMCache* cache;
	bool showPages = false;
	int i = 1;

	if (argc < 2 || !strcmp(argv[1], "--help")) {
		kprintf("usage: %s [-ps] <address>\n"
			"  if -p is specified, all pages are shown, if -s is used\n"
			"  only the cache info is shown respectively.\n", argv[0]);
		return 0;
	}
	while (argv[i][0] == '-') {
		char* arg = argv[i] + 1;
		while (arg[0]) {
			if (arg[0] == 'p')
				showPages = true;
			arg++;
		}
		i++;
	}
	if (argv[i] == nullptr) {
		kprintf("%s: invalid argument, pass address\n", argv[0]);
		return 0;
	}

	uintptr_t address = parse_expression(argv[i]);
	if (address == 0)
		return 0;

	cache = (VMCache*)address;

	cache->Dump(showPages);

	set_debug_variable("_sourceCache", (uintptr_t)cache->source);

	return 0;
}

static void
dump_cache_tree_recursively(VMCache* cache, int level,
	VMCache* highlightCache)
{
	// print this cache
	for (int i = 0; i < level; i++)
		kprintf("  ");
	if (cache == highlightCache)
		kprintf("%p <--\n", cache);
	else
		kprintf("%p\n", cache);

	// recursively print its consumers
	for (VMCache::ConsumerList::Iterator it = cache->consumers.GetIterator();
			VMCache* consumer = it.Next();) {
		dump_cache_tree_recursively(consumer, level + 1, highlightCache);
	}
}

static int
dump_cache_tree(int argc, char** argv)
{
	if (argc != 2 || !strcmp(argv[1], "--help")) {
		kprintf("usage: %s <address>\n", argv[0]);
		return 0;
	}

	uintptr_t address = parse_expression(argv[1]);
	if (address == 0)
		return 0;

	VMCache* cache = (VMCache*)address;
	VMCache* root = cache;

	// find the root cache (the transitive source)
	while (root->source != nullptr)
		root = root->source;

	dump_cache_tree_recursively(root, 0, cache);

	return 0;
}
#endif

void vm_init_mappings(void)
{
	gPageMappingsObjectCache = create_object_cache_etc("page mappings",
			sizeof(vm_page_mapping), 0, 0, 64, 128, CACHE_LARGE_SLAB, nullptr, nullptr,
			nullptr, nullptr);

	if(gPageMappingsObjectCache == nullptr)
		panic("Can't create page mapping object cache");

	object_cache_set_minimum_reserve(gPageMappingsObjectCache, 512);

#if defined(CONFIG_KDEBUGGER)
	add_debugger_command("areas", &dump_area_list, "Dump a list of all areas");
	add_debugger_command("area", &dump_area,
		"Dump info about a particular area");
	add_debugger_command("avail", &dump_available_memory,
		"Dump available memory");
	add_debugger_command("dl", &display_mem, "dump memory long words (64-bit)");
	add_debugger_command("dw", &display_mem, "dump memory words (32-bit)");
	add_debugger_command("ds", &display_mem, "dump memory shorts (16-bit)");
	add_debugger_command("db", &display_mem, "dump memory bytes (8-bit)");
	add_debugger_command("string", &display_mem, "dump strings");
	add_debugger_command_etc("mapping", &dump_mapping_info,
		"Print address mapping information",
		"[ \"-r\" | \"-p\" ] <address> [ <thread ID> ]\n"
		"Prints low-level page mapping information for a given address. If\n"
		"neither \"-r\" nor \"-p\" are specified, <address> is a virtual\n"
		"address that is looked up in the translation map of the current\n"
		"team, respectively the team specified by thread ID <thread ID>. If\n"
		"\"-r\" is specified, <address> is a physical address that is\n"
		"searched in the translation map of all teams, respectively the team\n"
		"specified by thread ID <thread ID>. If \"-p\" is specified,\n"
		"<address> is the address of a vm_page structure. The behavior is\n"
		"equivalent to specifying \"-r\" with the physical address of that\n"
		"page.\n",
		0);
	add_debugger_command("cache", &dump_cache, "Dump VMCache");
	add_debugger_command("cache_tree", &dump_cache_tree, "Dump VMCache tree");
#endif
}

#if defined(CONFIG_KDEBUGGER)
int vm_debug_copy_page_memory(pid_t teamID, void* unsafeMemory, void* buffer,
	size_t size, bool copyToUnsafe)
{
	if (size > PAGE_SIZE || rounddown2((uintptr_t)unsafeMemory, PAGE_SIZE)
			!= rounddown2((uintptr_t)unsafeMemory + size - 1, PAGE_SIZE)) {
		return -EINVAL;
	}

	// get the address space for the debugged thread
	VMAddressSpace * addressSpace;
	if (is_kernelspace_ptr_valid(unsafeMemory, size)) {
		addressSpace = VMAddressSpace::Kernel();
	} else if (teamID == B_CURRENT_TEAM) {
		struct thread* thread = debug_get_debugged_thread();
		if (thread == nullptr || thread->task == nullptr)
			return -EFAULT;

		addressSpace = thread->task->addressSpace;
	} else {
		addressSpace = VMAddressSpace::DebugGet(teamID);
	}

	if (addressSpace == nullptr)
		return -EFAULT;

	// get the area
	VMArea* area = addressSpace->LookupArea((uintptr_t)unsafeMemory);
	if (area == nullptr)
		return -EFAULT;

	// search the page
	off_t cacheOffset = (uintptr_t)unsafeMemory - area->Base() + area->cache_offset;
	VMCache* cache = area->cache;
	struct page* page = nullptr;
	while (cache != nullptr) {
		page = cache->DebugLookupPage(cacheOffset);
		if (page != nullptr)
			break;

		// Page not found in this cache -- if it is paged out, we must not try
		// to get it from lower caches.
		if (cache->DebugHasPage(cacheOffset))
			break;

		cache = cache->source;
	}

	if (page == nullptr)
		return -EOPNOTSUPP;

	// copy from/to physical memory
	vm_paddr_t physicalAddress = page_to_phys(page) + (uintptr_t)unsafeMemory % PAGE_SIZE;

	if (copyToUnsafe) {
		if (page->Cache() != area->cache)
			return -EOPNOTSUPP;

		return vm_memcpy_to_physical(physicalAddress, buffer, size, false);
	}

	return vm_memcpy_from_physical(buffer, physicalAddress, size, false);
}
#endif
