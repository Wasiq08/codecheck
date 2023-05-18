

#include <core/kconfig.h>
#include <core/debug.h>
#include <mm/mm_cache.h>
#include <mm/mm.h>
#include <core/thread.h>
#include <fs/vfs.h>
#include <core/wsm.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <lib/string.h>
#include <cinttypes>
#include <driver/nsrpc_tee_master.h>
#include <lib/RadixBitmap.h>
#include <mm/mm_resource.h>
#include <trustware/system_info.h>
#include <vfs/fd.h>
#include <trustware/drivers.h>
#include <mm/VMAddressSpace.h>
#include "mm_anonymous_cache.h"

//#define TRACE_SWAP		1

// number of free swap blocks the object cache shall minimally have
#define MIN_SWAP_BLOCK_RESERVE		128
#define INITIAL_SWAP_HASH_SIZE		256

#define SWAP_SLOT_NONE	RADIX_SLOT_NONE

#define SWAP_BLOCK_PAGES 32
#define SWAP_BLOCK_SHIFT 5		/* 1 << SWAP_BLOCK_SHIFT == SWAP_BLOCK_PAGES */
#define SWAP_BLOCK_MASK  (SWAP_BLOCK_PAGES - 1)

struct swap_file : DoublyLinkedListLinkImpl<swap_file> {
	int				fd;
	struct vnode*	vnode;
	void*			cookie;
	swap_addr_t		first_slot;
	swap_addr_t		last_slot;
	radix_bitmap*	bmp;
};

struct swap_hash_key {
	VMAnonymousCache	*cache;
	off_t				page_index;  // page index in the cache
};

// Each swap block contains swap address information for
// SWAP_BLOCK_PAGES continuous pages from the same cache
struct swap_block {
	swap_block*		hash_link;
	swap_hash_key	key;
	uint32_t		used;
	swap_addr_t		swap_slots[SWAP_BLOCK_PAGES];
};

struct SwapHashTableDefinition {
	typedef swap_hash_key KeyType;
	using ValueType = swap_block;

	[[nodiscard]] size_t HashKey(const swap_hash_key& key) const
	{
		off_t blockIndex = key.page_index >> SWAP_BLOCK_SHIFT;
		VMAnonymousCache* cache = key.cache;
		return blockIndex ^ (size_t)(int*)cache;
	}

	size_t Hash(const swap_block* value) const
	{
		return HashKey(value->key);
	}

	bool Compare(const swap_hash_key& key, const swap_block* value) const
	{
		return (key.page_index & ~(off_t)SWAP_BLOCK_MASK)
				== (value->key.page_index & ~(off_t)SWAP_BLOCK_MASK)
			&& key.cache == value->key.cache;
	}

	swap_block*& GetLink(swap_block* value) const
	{
		return value->hash_link;
	}
};

using SwapHashTable = BOpenHashTable<SwapHashTableDefinition>;
using SwapFileList = DoublyLinkedList<swap_file>;

static SwapHashTable *sSwapHashTable;
static cond_var_t sSwapHashResizeCondition { "swap hash resizer" };
static rw_lock_t sSwapHashLock { "swap hash" };

static SwapFileList sSwapFileList;
static mutex_t sSwapFileListLock { "swap file list" };
static swap_file *sSwapFileAlloc = nullptr; // allocate from here
static uint32_t sSwapFileCount = 0;

static off_t sAvailSwapSpace = 0;
static mutex_t sAvailSwapSpaceLock { "swap avail lock" };

static ObjectCache * sSwapBlockCache;

static swap_addr_t swap_slot_alloc(uint32_t count)
{
	mutex_lock(&sSwapFileListLock);

	if (sSwapFileList.IsEmpty()) {
		panic("swap_slot_alloc(): no swap file in the system\n");
	}

	// since radix bitmap could not handle more than 32 pages, we return
	// SWAP_SLOT_NONE, this forces Write() adjust allocation amount
	if (count > BITMAP_RADIX) {
		mutex_unlock(&sSwapFileListLock);
		return SWAP_SLOT_NONE;
	}

	swap_addr_t j, addr = SWAP_SLOT_NONE;
	for (j = 0; j < sSwapFileCount; j++) {
		if (sSwapFileAlloc == nullptr)
			sSwapFileAlloc = sSwapFileList.First();

		addr = radix_bitmap_alloc(sSwapFileAlloc->bmp, count);
		if (addr != SWAP_SLOT_NONE) {
			addr += sSwapFileAlloc->first_slot;
			break;
		}

		// this swap_file is full, find another
		sSwapFileAlloc = sSwapFileList.GetNext(sSwapFileAlloc);
	}

	if (j == sSwapFileCount) {
		panic("swap_slot_alloc: swap space exhausted!\n");
	}

	// if this swap file has used more than 90% percent of its space
	// switch to another
	if (sSwapFileAlloc->bmp->free_slots
		< (sSwapFileAlloc->last_slot - sSwapFileAlloc->first_slot) / 10) {
		sSwapFileAlloc = sSwapFileList.GetNext(sSwapFileAlloc);
	}

	mutex_unlock(&sSwapFileListLock);

	return addr;
}

static swap_file* find_swap_file(swap_addr_t slotIndex)
{
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
		swap_file* swapFile = it.Next();) {
		if (slotIndex >= swapFile->first_slot
			&& slotIndex < swapFile->last_slot) {
			return swapFile;
		}
	}

	panic("find_swap_file(): can't find swap file for slot %" PRIu32 "\n",
		slotIndex);
}

static void
swap_slot_dealloc(swap_addr_t slotIndex, uint32_t count)
{
	if (slotIndex == SWAP_SLOT_NONE)
		return;

	mutex_lock(&sSwapFileListLock);
	swap_file* swapFile = find_swap_file(slotIndex);
	slotIndex -= swapFile->first_slot;
	radix_bitmap_dealloc(swapFile->bmp, slotIndex, count);
	fs_trim_data data;
	data.range_count = 1;
	data.trimmed_size = count * PAGE_SIZE;
	data.ranges[0].offset = slotIndex * PAGE_SIZE;
	data.ranges[0].size = count * PAGE_SIZE;
	_kern_ioctl(swapFile->fd, B_TRIM_DEVICE, &data);
	mutex_unlock(&sSwapFileListLock);
}

static off_t
swap_space_reserve(off_t amount)
{
	mutex_lock(&sAvailSwapSpaceLock);
	if (sAvailSwapSpace >= amount)
		sAvailSwapSpace -= amount;
	else {
		amount = sAvailSwapSpace;
		sAvailSwapSpace = 0;
	}
	mutex_unlock(&sAvailSwapSpaceLock);

	return amount;
}

static void
swap_space_unreserve(off_t amount)
{
	mutex_lock(&sAvailSwapSpaceLock);
	sAvailSwapSpace += amount;
	mutex_unlock(&sAvailSwapSpaceLock);
}


class VMAnonymousCache::WriteCallback : public StackableAsyncIOCallback {
public:
	WriteCallback(VMAnonymousCache* cache, AsyncIOCallback* callback)
		:
		StackableAsyncIOCallback(callback),
		fCache(cache)
	{
	}

	void SetTo(vm_pindex_t pageIndex, swap_addr_t slotIndex, bool newSlot)
	{
		fPageIndex = pageIndex;
		fSlotIndex = slotIndex;
		fNewSlot = newSlot;
	}

	void IOFinished(int status, bool partialTransfer,
		size_t bytesTransferred) override
	{
		if (fNewSlot) {
			if (status == 0) {
				fCache->_SwapBlockBuild(fPageIndex, fSlotIndex, 1);
			} else {
				AutoLocker<VMCache> locker(fCache);
				fCache->allocated_swap_size -= PAGE_SIZE;
				locker.Unlock();

				swap_slot_dealloc(fSlotIndex, 1);
			}
		}

		fNextCallback->IOFinished(status, partialTransfer, bytesTransferred);

		delete this;
	}

private:
	VMAnonymousCache*	fCache;
	vm_pindex_t			fPageIndex = 0;
	swap_addr_t			fSlotIndex = 0;
	bool				fNewSlot = false;
};

VMAnonymousCache::~VMAnonymousCache() {
	_FreeSwapPageRange(virtual_base, virtual_end, false);
	swap_space_unreserve(committed_swap_size);
	if (committed_size > committed_swap_size)
		vm_unreserve_memory(committed_size - committed_swap_size);
}

int VMAnonymousCache::Init(bool canOvercommit, int32_t numPrecommittedPages,
		int32_t numGuardPages, uint32_t allocationFlags) {
	int error = VMCache::Init(CACHE_TYPE_RAM, allocationFlags);

	if (error)
		return error;

	can_overcommit = canOvercommit;
	has_precommitted = false;
	precommitted_pages = MIN(numPrecommittedPages, 255);
	guard_size = numGuardPages * PAGE_SIZE;
	committed_swap_size = 0;
	allocated_swap_size = 0;
	mergeable = true;

	return 0;
}

void
VMAnonymousCache::_FreeSwapPageRange(off_t fromOffset, off_t toOffset,
	bool skipBusyPages)
{
	swap_block* swapBlock = NULL;
	off_t toIndex = toOffset >> PAGE_SHIFT;
	for (off_t pageIndex = fromOffset >> PAGE_SHIFT;
		pageIndex < toIndex && allocated_swap_size > 0; pageIndex++) {

		WriteLocker locker(sSwapHashLock);

		// Get the swap slot index for the page.
		swap_addr_t blockIndex = pageIndex & SWAP_BLOCK_MASK;
		if (swapBlock == NULL || blockIndex == 0) {
			swap_hash_key key = { this, pageIndex };
			swapBlock = sSwapHashTable->Lookup(key);

			if (swapBlock == NULL) {
				pageIndex = roundup(pageIndex + 1, SWAP_BLOCK_PAGES) - 1;
				continue;
			}
		}

		swap_addr_t slotIndex = swapBlock->swap_slots[blockIndex];
		if (slotIndex == SWAP_SLOT_NONE)
			continue;

		if (skipBusyPages) {
			auto page = LookupPage(pageIndex * PAGE_SIZE);
			if (page != NULL && page->busy) {
				// TODO: We skip (i.e. leak) swap space of busy pages, since
				// there could be I/O going on (paging in/out). Waiting is
				// not an option as 1. unlocking the cache means that new
				// swap pages could be added in a range we've already
				// cleared (since the cache still has the old size) and 2.
				// we'd risk a deadlock in case we come from the file cache
				// and the FS holds the node's write-lock. We should mark
				// the page invalid and let the one responsible clean up.
				// There's just no such mechanism yet.
				continue;
			}
		}

		swap_slot_dealloc(slotIndex, 1);
		allocated_swap_size -= PAGE_SIZE;

		swapBlock->swap_slots[blockIndex] = SWAP_SLOT_NONE;
		if (--swapBlock->used == 0) {
			// All swap pages have been freed -- we can discard the swap block.
			sSwapHashTable->RemoveUnchecked(swapBlock);
			object_cache_free(sSwapBlockCache, swapBlock,
				CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL);

			// There are no swap pages for possibly remaining pages, skip to the
			// next block.
			pageIndex = roundup(pageIndex + 1, SWAP_BLOCK_PAGES) - 1;
			swapBlock = NULL;
		}
	}
}

int
VMAnonymousCache::Resize(off_t newSize, int priority)
{
	_FreeSwapPageRange(newSize + PAGE_SIZE - 1, virtual_end + PAGE_SIZE - 1);
	return VMCache::Resize(newSize, priority);
}

int
VMAnonymousCache::Rebase(off_t newBase, int priority)
{
	_FreeSwapPageRange(virtual_base, newBase);
	return VMCache::Rebase(newBase, priority);
}

/*!	Moves the swap pages for the given range from the source cache into this
	cache. Both caches must be locked.
*/
int
VMAnonymousCache::Adopt(VMCache* _source, off_t offset, off_t size,
	off_t newOffset)
{
	VMAnonymousCache* anonSourceCache = dynamic_cast<VMAnonymousCache*>(_source);
	if (anonSourceCache == nullptr) {
		panic("VMAnonymousCache::Adopt(): adopt from incompatible cache %p "
			"requested", _source);
	}

	off_t pageIndex = newOffset >> PAGE_SHIFT;
	off_t sourcePageIndex = offset >> PAGE_SHIFT;
	off_t sourceEndPageIndex = (offset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	swap_block* swapBlock = NULL;

	WriteLocker locker(sSwapHashLock);

	while (sourcePageIndex < sourceEndPageIndex
	       && anonSourceCache->allocated_swap_size > 0) {
		swap_addr_t left
			= SWAP_BLOCK_PAGES - (sourcePageIndex & SWAP_BLOCK_MASK);

		swap_hash_key sourceKey = {anonSourceCache, sourcePageIndex };
		swap_block* sourceSwapBlock = sSwapHashTable->Lookup(sourceKey);
		if (sourceSwapBlock == NULL || sourceSwapBlock->used == 0) {
			sourcePageIndex += left;
			pageIndex += left;
			swapBlock = NULL;
			continue;
		}

		for (; left > 0 && sourceSwapBlock->used > 0;
				left--, sourcePageIndex++, pageIndex++) {

			swap_addr_t blockIndex = pageIndex & SWAP_BLOCK_MASK;
			if (swapBlock == NULL || blockIndex == 0) {
				swap_hash_key key = { this, pageIndex };
				swapBlock = sSwapHashTable->Lookup(key);

				if (swapBlock == NULL) {
					swapBlock = (swap_block*)object_cache_alloc(sSwapBlockCache,
						CACHE_DONT_WAIT_FOR_MEMORY
							| CACHE_DONT_LOCK_KERNEL);
					if (swapBlock == NULL)
						return -ENOMEM;

					swapBlock->key.cache = this;
					swapBlock->key.page_index
						= pageIndex & ~(off_t)SWAP_BLOCK_MASK;
					swapBlock->used = 0;
					for (uint32_t i = 0; i < SWAP_BLOCK_PAGES; i++)
						swapBlock->swap_slots[i] = SWAP_SLOT_NONE;

					sSwapHashTable->InsertUnchecked(swapBlock);
				}
			}

			swap_addr_t sourceBlockIndex = sourcePageIndex & SWAP_BLOCK_MASK;
			swap_addr_t slotIndex
				= sourceSwapBlock->swap_slots[sourceBlockIndex];
			if (slotIndex == SWAP_SLOT_NONE)
				continue;

			ASSERT(swapBlock->swap_slots[blockIndex] == SWAP_SLOT_NONE);

			swapBlock->swap_slots[blockIndex] = slotIndex;
			swapBlock->used++;
			allocated_swap_size += PAGE_SIZE;

			sourceSwapBlock->swap_slots[sourceBlockIndex] = SWAP_SLOT_NONE;
			sourceSwapBlock->used--;
			anonSourceCache->allocated_swap_size -= PAGE_SIZE;
		}

		if (left > 0) {
			sourcePageIndex += left;
			pageIndex += left;
			swapBlock = NULL;
		}

		if (sourceSwapBlock->used == 0) {
			// All swap pages have been adopted, we can discard the swap block.
			sSwapHashTable->RemoveUnchecked(sourceSwapBlock);
			object_cache_free(sSwapBlockCache, sourceSwapBlock,
					CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL);
		}
	}

	locker.Unlock();

	return VMCache::Adopt(anonSourceCache, offset, size, newOffset);
}


int VMAnonymousCache::Commit(off_t size, int priority)
{
	// If we can overcommit, we don't commit here, but in Fault(). We always
	// unreserve memory, if we're asked to shrink our commitment, though.
	if (can_overcommit && size > committed_size) {
		if (has_precommitted)
			return 0;

		// pre-commit some pages to make a later failure less probable
		has_precommitted = true;
		off_t precommitted = precommitted_pages * PAGE_SIZE;
		if (size > precommitted)
			size = precommitted;
	}

	return _Commit(size, priority);
}

bool VMAnonymousCache::HasPage(off_t offset)
{
	if (_SwapBlockGetAddress(offset >> PAGE_SHIFT) != SWAP_SLOT_NONE)
		return true;

	return false;
}

bool VMAnonymousCache::CanWritePage(off_t offset)
{
	// We can write the page, if we have not used all of our committed swap
	// space or the page already has a swap slot assigned.
	return allocated_swap_size < committed_swap_size
		|| _SwapBlockGetAddress(offset >> PAGE_SHIFT) != SWAP_SLOT_NONE;
}

int VMAnonymousCache::Fault(struct VMAddressSpace *aspace, off_t offset)
{
	if (guard_size > 0) {
		off_t guardOffset;

		guardOffset = 0;

		// report stack fault, guard page hit!
		if (offset >= guardOffset && offset < guardOffset + guard_size) {
			return -EFAULT;
		}
	}

	if (can_overcommit && LookupPage(offset) == nullptr && !HasPage(offset)) {
		if (precommitted_pages == 0) {
			// never commit more than needed
			if (committed_size / PAGE_SIZE > (off_t)page_count)
				return VM_FAULT_DONT_HANDLE;

			// try to commit additional swap space/memory
			if (swap_space_reserve(PAGE_SIZE) == PAGE_SIZE) {
				committed_swap_size += PAGE_SIZE;
			} else {
				int priority = aspace == VMAddressSpace::Kernel() ? GFP_PRIO_SYSTEM : GFP_PRIO_USER;

				if(vm_try_reserve_memory(PAGE_SIZE, priority, 0) != 0) {
					static struct timeval log_time;
					static int log_pps;

					if (ppsratecheck(&log_time, &log_pps, 1)) {
						printk(KERN_ERR,
								"VMAnonymousCache: Failed to reserve %d bytes of RAM\n",
								PAGE_SIZE);
					}

					return -ENOMEM;
				}
			}

			committed_size += PAGE_SIZE;
		} else {
			precommitted_pages--;
		}
	}

	// This will cause vm_soft_fault() to handle the fault
	return VM_FAULT_DONT_HANDLE;
}

void VMAnonymousCache::Merge(VMCache* _source)
{
	VMAnonymousCache* sourceToMerge = dynamic_cast<VMAnonymousCache*>(_source);
	if (sourceToMerge == nullptr) {
		panic("VMAnonymousCache::Merge(): merge with incompatible cache "
			"%p requested", _source);
	}

	// take over the source' committed size
	committed_swap_size += sourceToMerge->committed_swap_size;
	sourceToMerge->committed_swap_size = 0;
	committed_size += sourceToMerge->committed_size;
	sourceToMerge->committed_size = 0;

	off_t actualSize = virtual_end - virtual_base;
	if (committed_size > actualSize)
		_Commit(actualSize, GFP_PRIO_USER);

	// Move all not shadowed swap pages from the source to the consumer cache.
	// Also remove all source pages that are shadowed by consumer swap pages.
	_MergeSwapPages(sourceToMerge);

	// Move all not shadowed pages from the source to the consumer cache.
	if (sourceToMerge->page_count < page_count)
		_MergePagesSmallerSource(sourceToMerge);
	else
		_MergePagesSmallerConsumer(sourceToMerge);
}

off_t VMAnonymousCache::Read(off_t offset, const generic_iovec * vecs, uint32_t flags, size_t count,
		size_t  *_numBytes)
{
	off_t pageIndex = offset >> PAGE_SHIFT;

	for (uint32_t i = 0, j = 0; i < count; i = j) {
		swap_addr_t startSlotIndex = _SwapBlockGetAddress(pageIndex + i);
		for (j = i + 1; j < count; j++) {
			swap_addr_t slotIndex = _SwapBlockGetAddress(pageIndex + j);
			if (slotIndex != startSlotIndex + j - i)
				break;
		}

		// TODO: Assumes that only one page is read.

		mutex_lock(&sSwapFileListLock);
		swap_file* swapFile = find_swap_file(startSlotIndex);
		mutex_unlock(&sSwapFileListLock);

		off_t pos = (off_t)(startSlotIndex - swapFile->first_slot) * PAGE_SIZE;

		int status = vfs_read_pages(swapFile->vnode, swapFile->cookie, pos,
			vecs + i, j - i, flags, _numBytes);

		if (status != 0)
			return status;
	}

	return 0;
}

off_t VMAnonymousCache::Write(off_t offset, const generic_iovec * vecs, uint32_t flags, size_t count,
		size_t  *)
{
	off_t pageIndex = offset >> PAGE_SHIFT;

	AutoLocker<VMCache> locker(this);

	off_t totalPages = 0;
	for (uint32_t i = 0; i < count; i++) {
		off_t pageCount = (vecs[i].length + PAGE_SIZE - 1) >> PAGE_SHIFT;
		swap_addr_t slotIndex = _SwapBlockGetAddress(pageIndex + totalPages);
		if (slotIndex != SWAP_SLOT_NONE) {
			swap_slot_dealloc(slotIndex, pageCount);
			_SwapBlockFree(pageIndex + totalPages, pageCount);
			allocated_swap_size -= pageCount * PAGE_SIZE;
		}

		totalPages += pageCount;
	}

	off_t totalSize = totalPages * PAGE_SIZE;
	if (allocated_swap_size + totalSize > committed_swap_size)
		return -EINVAL;

	allocated_swap_size += totalSize;
	locker.Unlock();

	off_t pagesLeft = totalPages;
	totalPages = 0;

	for (uint32_t i = 0; i < count; i++) {
		off_t pageCount = (vecs[i].length + PAGE_SIZE - 1) >> PAGE_SHIFT;

		uint64_t vectorBase = vecs[i].base;
		size_t vectorLength = vecs[i].length;
		off_t n = pageCount;

		for (off_t j = 0; j < pageCount; j += n) {
			swap_addr_t slotIndex;
			// try to allocate n slots, if fail, try to allocate n/2
			while ((slotIndex = swap_slot_alloc(n)) == SWAP_SLOT_NONE && n >= 2)
				n >>= 1;

			if (slotIndex == SWAP_SLOT_NONE)
				panic("VMAnonymousCache::Write(): can't allocate swap space\n");

			// TODO: Assumes that only one page is written.

			mutex_lock(&sSwapFileListLock);
			swap_file* swapFile = find_swap_file(slotIndex);
			mutex_unlock(&sSwapFileListLock);

			off_t pos = (off_t)(slotIndex - swapFile->first_slot) * PAGE_SIZE;

			size_t length = (size_t)n * PAGE_SIZE;

			generic_iovec vector[1];
			vector[0].base = vectorBase;
			vector[0].length = length;

			int status = vfs_write_pages(swapFile->vnode, swapFile->cookie,
				pos, vector, 1, flags, &length);

			if (status < 0) {
				locker.Lock();
				allocated_swap_size -= (off_t)pagesLeft * PAGE_SIZE;
				locker.Unlock();

				swap_slot_dealloc(slotIndex, n);
				return status;
			}

			_SwapBlockBuild(pageIndex + totalPages, slotIndex, n);
			pagesLeft -= n;

			if (n != pageCount) {
				vectorBase += n * PAGE_SIZE;
				vectorLength -= n * PAGE_SIZE;
			}
		}

		totalPages += pageCount;
	}

	ASSERT(pagesLeft == 0);
	return 0;
}

int VMAnonymousCache::WriteAsync(off_t offset, const struct generic_iovec* vecs,
			[[maybe_unused]] size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* _callback)
{
	// TODO: Currently this method is only used for single pages. Either make
	// more flexible use of it or change the interface!
	// This implementation relies on the current usage!
	ASSERT(count == 1);
	ASSERT(numBytes <= PAGE_SIZE);

	vm_pindex_t pageIndex = offset >> PAGE_SHIFT;
	swap_addr_t slotIndex = _SwapBlockGetAddress(pageIndex);
	bool newSlot = slotIndex == SWAP_SLOT_NONE;

	// If the page doesn't have any swap space yet, allocate it.
	if (newSlot) {
		AutoLocker<VMCache> locker(this);
		if (allocated_swap_size + PAGE_SIZE > committed_swap_size) {
			_callback->IOFinished(-EIO, true, 0);
			return -EIO;
		}

		allocated_swap_size += PAGE_SIZE;

		slotIndex = swap_slot_alloc(1);
	}

	// create our callback
	WriteCallback* callback = (flags & GENERIC_IO_VIP) != 0
		? new(std::nothrow, wait_flags_t(CACHE_USE_RESERVE)) WriteCallback(this, _callback)
		: new(std::nothrow) WriteCallback(this, _callback);
	if (callback == nullptr) {
		if (newSlot) {
			AutoLocker<VMCache> locker(this);
			allocated_swap_size -= PAGE_SIZE;
			locker.Unlock();

			swap_slot_dealloc(slotIndex, 1);
		}
		_callback->IOFinished(-ENOMEM, true, 0);
		return -ENOMEM;
	}
	// TODO: If the page already had swap space assigned, we don't need an own
	// callback.

	callback->SetTo(pageIndex, slotIndex, newSlot);

	// write the page asynchrounously
	mutex_lock(&sSwapFileListLock);
	swap_file* swapFile = find_swap_file(slotIndex);
	mutex_unlock(&sSwapFileListLock);

	off_t pos = (off_t)(slotIndex - swapFile->first_slot) * PAGE_SIZE;

	return vfs_asynchronous_write_pages(swapFile->vnode, swapFile->cookie, pos,
		vecs, 1, numBytes, flags, callback);
}

int VMAnonymousCache::MaxPagesPerAsyncWrite() const
{
	return 1;
}

int VMAnonymousCache::MaxPagesPerWrite() const
{
	return -1;
}

void VMAnonymousCache::DeleteObject()
{
	object_cache_delete(gVMCacheObjectCache, this);
}

void VMAnonymousCache::_SwapBlockBuild(off_t startPageIndex, swap_addr_t startSlotIndex, uint32_t count)
{
	WriteLocker locker(sSwapHashLock);

	bool neededResizeBefore = sSwapHashTable->ResizeNeededGuess();

	uint32_t left = count;
	for (uint32_t i = 0, j = 0; i < count; i += j) {
		off_t pageIndex = startPageIndex + i;
		swap_addr_t slotIndex = startSlotIndex + i;

		swap_hash_key key = { this, pageIndex };

		swap_block* swap = sSwapHashTable->Lookup(key);
		while (swap == nullptr) {
			swap = (swap_block*)object_cache_alloc(sSwapBlockCache, CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY);

			if (swap == nullptr) {
				// Wait a short time until memory is available again.
				locker.Unlock();
				object_cache_reserve(sSwapBlockCache, 1, CACHE_DONT_OOM_KILL);
				locker.Lock();
				swap = sSwapHashTable->Lookup(key);
				continue;
			}

			swap->key.cache = this;
			swap->key.page_index = pageIndex & ~(off_t)SWAP_BLOCK_MASK;
			swap->used = 0;
			for (unsigned int & swap_slot : swap->swap_slots)
				swap_slot = SWAP_SLOT_NONE;

			sSwapHashTable->InsertUnchecked(swap);
		}

		swap_addr_t blockIndex = pageIndex & SWAP_BLOCK_MASK;
		for (j = 0; blockIndex < SWAP_BLOCK_PAGES && left > 0; j++) {
			swap->swap_slots[blockIndex++] = slotIndex + j;
			left--;
		}

		swap->used += j;
	}

	bool needsResizeNow = sSwapHashTable->ResizeNeededGuess();

	if(!neededResizeBefore && needsResizeNow) {
		sSwapHashResizeCondition.NotifyOne();
	}
}

void VMAnonymousCache::_SwapBlockFree(off_t startPageIndex, uint32_t count)
{
	WriteLocker locker(sSwapHashLock);

	uint32_t left = count;
	for (uint32_t i = 0, j = 0; i < count; i += j) {
		off_t pageIndex = startPageIndex + i;
		swap_hash_key key = { this, pageIndex };
		swap_block* swap = sSwapHashTable->Lookup(key);

		ASSERT(swap != nullptr);

		swap_addr_t blockIndex = pageIndex & SWAP_BLOCK_MASK;
		for (j = 0; blockIndex < SWAP_BLOCK_PAGES && left > 0; j++) {
			swap->swap_slots[blockIndex++] = SWAP_SLOT_NONE;
			left--;
		}

		swap->used -= j;
		if (swap->used == 0) {
			sSwapHashTable->RemoveUnchecked(swap);
			object_cache_free(sSwapBlockCache, swap, CACHE_DONT_LOCK_KERNEL|CACHE_DONT_WAIT_FOR_MEMORY);
		}
	}
}

swap_addr_t VMAnonymousCache::_SwapBlockGetAddress(off_t pageIndex)
{
	ReadLocker locker(sSwapHashLock);

	swap_hash_key key = { this, pageIndex };
	swap_block* swap = sSwapHashTable->Lookup(key);
	swap_addr_t slotIndex = SWAP_SLOT_NONE;

	if (swap != nullptr) {
		swap_addr_t blockIndex = pageIndex & SWAP_BLOCK_MASK;
		slotIndex = swap->swap_slots[blockIndex];
	}

	return slotIndex;
}

int VMAnonymousCache::_Commit(off_t size, int priority)
{
#ifdef TRACE_SWAP
	printk(KERN_ERR|WITHUART, "%p->VMAnonymousCache::_Commit(%zd), already committed: "
		"%zd (%zd swap), %zd free RAM\n", this, (size_t)size, (size_t)committed_size,
		(size_t)committed_swap_size, vm_available_memory());
#endif

	// Basic strategy: reserve swap space first, only when running out of swap
	// space, reserve real memory.

	off_t committedMemory = committed_size - committed_swap_size;

	// Regardless of whether we're asked to grow or shrink the commitment,
	// we always try to reserve as much as possible of the final commitment
	// in the swap space.
	if (size > committed_swap_size) {
		committed_swap_size += swap_space_reserve(size - committed_swap_size);
		committed_size = committed_swap_size + committedMemory;
#if 0
		if (size > committed_swap_size) {
			printk(KERN_DEBUG, "%p->VMAnonymousCache::_Commit(%zd), reserved "
				"only %zd swap\n", this, (size_t)size, (size_t)committed_swap_size);
		}
#endif
	}

	if (committed_size == size)
		return 0;

	if (committed_size > size) {
		// The commitment shrinks -- unreserve real memory first.
		off_t toUnreserve = committed_size - size;
		if (committedMemory > 0) {
			off_t unreserved = MIN(toUnreserve, committedMemory);
			vm_unreserve_memory(unreserved);
			committedMemory -= unreserved;
			committed_size -= unreserved;
			toUnreserve -= unreserved;
		}

		// Unreserve swap space.
		if (toUnreserve > 0) {
			swap_space_unreserve(toUnreserve);
			committed_swap_size -= toUnreserve;
			committed_size -= toUnreserve;
		}

		return 0;
	}

	// The commitment grows -- we have already tried to reserve swap space at
	// the start of the method, so we try to reserve real memory, now.

	off_t toReserve = size - committed_size;
	if (vm_try_reserve_memory(toReserve, priority, SECONDS(1)) < 0) {
		static struct timeval log_time;
		static int log_pps;

		if (ppsratecheck(&log_time, &log_pps, 1)) {
			printk(KERN_INFO, "%p->VMAnonymousCache::_Commit(%zd): Failed to "
					"reserve %zd bytes of RAM\n", this, (size_t )size,
					(size_t )toReserve);
		}
		return -ENOMEM;
	}

	committed_size = size;
	return 0;
}

void
VMAnonymousCache::_MergePagesSmallerSource(VMAnonymousCache* sourceToMerge)
{
	// The source cache has less pages than the consumer (this cache), so we
	// iterate through the source's pages and move the ones that are not
	// shadowed up to the consumer.

	for (VMCachePagesTree::Iterator it = sourceToMerge->pages.GetIterator();
			struct page* page = it.Next();) {
		// Note: Removing the current node while iterating through a
		// IteratableSplayTree is safe.
		struct page* consumerPage = LookupPage(
			(off_t)page->cache_offset << PAGE_SHIFT);
		if (consumerPage == nullptr) {
			// the page is not yet in the consumer cache - move it upwards
			ASSERT(!page->busy);
			MovePage(page);
		}
	}
}

void VMAnonymousCache::_MergePagesSmallerConsumer(VMAnonymousCache* sourceToMerge)
{
	// The consumer (this cache) has less pages than the source, so we move the
	// consumer's pages to the source (freeing shadowed ones) and finally just
	// all pages of the source back to the consumer.

	for (VMCachePagesTree::Iterator it = pages.GetIterator();
		auto* page = it.Next();) {
		// If a source page is in the way, remove and free it.
		auto* sourcePage = sourceToMerge->LookupPage(
			(off_t)page->cache_offset << PAGE_SHIFT);
		if (sourcePage != nullptr) {
			ASSERT(!sourcePage->busy);
			ASSERT(sourcePage->WiredCount() == 0);
			ASSERT(sourcePage->mappings.IsEmpty());
			sourceToMerge->RemovePage(sourcePage);
			vm_page_free_etc(sourceToMerge, sourcePage, nullptr);
		}

		// Note: Removing the current node while iterating through a
		// IteratableSplayTree is safe.
		sourceToMerge->MovePage(page);
	}

	MoveAllPages(sourceToMerge);
}

void VMAnonymousCache::_MergeSwapPages(VMAnonymousCache* sourceToMerge)
{
	// If neither source nor consumer have swap pages, we don't have to do
	// anything.
	if (sourceToMerge->allocated_swap_size == 0 && allocated_swap_size == 0)
		return;

	for (off_t offset = sourceToMerge->virtual_base
	                    & ~(off_t)(PAGE_SIZE * SWAP_BLOCK_PAGES - 1);
	     offset < sourceToMerge->virtual_end;
		offset += PAGE_SIZE * SWAP_BLOCK_PAGES) {

		WriteLocker locker(sSwapHashLock);

		off_t swapBlockPageIndex = offset >> PAGE_SHIFT;
		swap_hash_key key = {sourceToMerge, swapBlockPageIndex };
		swap_block* sourceSwapBlock = sSwapHashTable->Lookup(key);

		// remove the source swap block -- we will either take over the swap
		// space (and the block) or free it
		if (sourceSwapBlock != nullptr)
			sSwapHashTable->RemoveUnchecked(sourceSwapBlock);

		key.cache = this;
		swap_block* swapBlock = sSwapHashTable->Lookup(key);

		locker.Unlock();

		// remove all source pages that are shadowed by consumer swap pages
		if (swapBlock != nullptr) {
			for (uint32_t i = 0; i < SWAP_BLOCK_PAGES; i++) {
				if (swapBlock->swap_slots[i] != SWAP_SLOT_NONE) {
					struct page * page = sourceToMerge->LookupPage(
						(off_t)(swapBlockPageIndex + i) << PAGE_SHIFT);
					if (page != nullptr) {
						ASSERT(!page->busy);
						sourceToMerge->RemovePage(page);
						vm_page_free_etc(sourceToMerge, page, nullptr);
					}
				}
			}
		}

		if (sourceSwapBlock == nullptr)
			continue;

		for (uint32_t i = 0; i < SWAP_BLOCK_PAGES; i++) {
			off_t pageIndex = swapBlockPageIndex + i;
			swap_addr_t sourceSlotIndex = sourceSwapBlock->swap_slots[i];

			if (sourceSlotIndex == SWAP_SLOT_NONE)
				continue;

			if ((swapBlock != nullptr
					&& swapBlock->swap_slots[i] != SWAP_SLOT_NONE)
				|| LookupPage((off_t)pageIndex << PAGE_SHIFT) != nullptr) {
				// The consumer already has a page or a swapped out page
				// at this index. So we can free the source swap space.
				swap_slot_dealloc(sourceSlotIndex, 1);
				sourceSwapBlock->swap_slots[i] = SWAP_SLOT_NONE;
				sourceSwapBlock->used--;
			}

			// We've either freed the source swap page or are going to move it
			// to the consumer. At any rate, the source cache doesn't own it
			// anymore.
			sourceToMerge->allocated_swap_size -= PAGE_SIZE;
		}

		// All source swap pages that have not been freed yet are taken over by
		// the consumer.
		allocated_swap_size += PAGE_SIZE * (off_t)sourceSwapBlock->used;

		if (sourceSwapBlock->used == 0) {
			// All swap pages have been freed -- we can discard the source swap
			// block.
			object_cache_free(sSwapBlockCache, sourceSwapBlock, CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY);
		} else if (swapBlock == nullptr) {
			// We need to take over some of the source's swap pages and there's
			// no swap block in the consumer cache. Just take over the source
			// swap block.
			sourceSwapBlock->key.cache = this;
			locker.Lock();
			sSwapHashTable->InsertUnchecked(sourceSwapBlock);
			locker.Unlock();
		} else {
			// We need to take over some of the source's swap pages and there's
			// already a swap block in the consumer cache. Copy the respective
			// swap addresses and discard the source swap block.
			for (uint32_t i = 0; i < SWAP_BLOCK_PAGES; i++) {
				if (sourceSwapBlock->swap_slots[i] != SWAP_SLOT_NONE)
					swapBlock->swap_slots[i] = sourceSwapBlock->swap_slots[i];
			}

			object_cache_free(sSwapBlockCache, sourceSwapBlock, CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY);
		}
	}
}

static int swap_hash_resizer(void*)
{
	size_t size;
	void* allocation;

	for(;;) {
		WriteLocker locker(sSwapHashLock);
		size = sSwapHashTable->ResizeNeeded();

		if (size == 0) {
			DEFINE_CONDVAR_ENTRY(entry);
			sSwapHashResizeCondition.Add(&entry);
			locker.Unlock();
			entry.Wait();
			continue;
		}

		locker.Unlock();

		allocation = sSwapHashTable->GetAllocator().Allocate(size);

		locker.Lock();

		if (allocation == nullptr) {
			DEFINE_CONDVAR_ENTRY(entry);
			sSwapHashResizeCondition.Add(&entry);
			locker.Unlock();
			entry.Wait();
			continue;
		}

		sSwapHashTable->Resize(allocation, size);
	}

	return 0;
}

int
VMAnonymousCache::Discard(off_t offset, off_t size)
{
	_FreeSwapPageRange(offset, offset + size);
	return VMCache::Discard(offset, size);
}

#if defined(CONFIG_KDEBUGGER)
bool
VMAnonymousCache::DebugHasPage(off_t offset) {
	off_t pageIndex = offset >> PAGE_SHIFT;
	swap_hash_key key = {this, pageIndex};
	swap_block *swap = sSwapHashTable->Lookup(key);
	if (swap == nullptr)
		return false;

	return swap->swap_slots[pageIndex & SWAP_BLOCK_MASK] != SWAP_SLOT_NONE;
}
#endif

#if 0
static int swap_file_manager(void *)
{
	nsrpc_rpc_data rpc = {};

	static const size_t kSwapBlockSize = 32 * 1024 * 1024;

	rpc.channel = NSRPC_CHANNEL_WSM;
	rpc.command = 1;
	rpc.arg[0] = kSwapBlockSize;

	// Try to allocate new swap
	int error = nsrpc_submit_kern(&rpc);

	if(error < 0) {
		printk(KERN_ERR|WITHUART, "Can't submit NSRPC request (error = %d)\n", error);
		return 0;
	}

	printk(KERN_ERR|WITHUART, "wait for RPC completion\n");

	error = nsrpc_wait_for_completion_kern(&rpc);

	if(error < 0) {
		printk(KERN_ERR|WITHUART, "Can't wait for NSRPC request (error = %d)\n", error);
		return 0;
	}

	wsm_node * wsm = wsm_translate_node(rpc.arg[0]);

	if(!wsm) {
		printk(KERN_ERR|WITHUART, "Can't find WSM %d which has been provided by NS\n", rpc.arg[0]);
		return 0;
	}

	printk(KERN_WARNING | WITHUART, "Allocated swap WSM with id %d and %zd bytes\n", rpc.arg[0], wsm->size);

	swap_file_wsm_block * swapFile = new(std::nothrow) swap_file_wsm_block(wsm);

	// Acquired in swap_file_wsm_block()
	kref_put(&wsm->kref);

	if(swapFile) {
		swapFile->ree_ref[0] = rpc.arg[1];
		swapFile->ree_ref[1] = rpc.arg[2];

		error = swapFile->BaseConstruct(swapFile->wsm->size / PAGE_SIZE);

		if(error < 0) {
			printk(KERN_ERR, "No memory to construct swap object\n");
			delete swapFile;
			swapFile = nullptr;
		}
	} else {
		printk(KERN_ERR|WITHUART, "No memory to allocate swap object\n");
	}

	if(!swapFile) {
		error = nsrpc_submit_kern(&rpc);

		if(error < 0) {
			printk(KERN_ERR|WITHUART, "Can't submit NSRPC request (error = %d). REE memory resource will leak\n", error);
			return 0;
		}

		error = nsrpc_wait_for_completion_kern(&rpc);

		if (error < 0) {
			printk(KERN_ERR|WITHUART, "Can't wait for NSRPC request (error = %d). REE memory resource may leak\n", error);
		}

		return 0;
	}

	return 0;
}
#endif

uint32_t swap_available_pages()
{
	mutex_lock(&sAvailSwapSpaceLock);
	uint32_t avail = sAvailSwapSpace >> PAGE_SHIFT;
	mutex_unlock(&sAvailSwapSpaceLock);

	return avail;
}


uint32_t swap_total_swap_pages()
{
	mutex_lock(&sSwapFileListLock);

	uint32_t totalSwapSlots = 0;
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
		swap_file* swapFile = it.Next();) {
		totalSwapSlots += swapFile->last_slot - swapFile->first_slot;
	}

	mutex_unlock(&sSwapFileListLock);

	return totalSwapSlots;
}

int swap_file_add(const char *path) {
	// open the file
	int fd = _kern_open_etc(-1, path, O_RDWR | O_NOCACHE, S_IRUSR | S_IWUSR);

	if (fd < 0)
		return fd;

	// fstat() it and check whether we can use it
	struct stat st;

	int error = _kern_read_stat(fd, nullptr, false, &st, sizeof(st));

	if (error < 0) {
		_kern_close(fd);
		return error;
	}

	if (S_ISCHR(st.st_mode) && st.st_size == 0) {
		device_geometry geometry;

		error = _kern_ioctl(fd, B_GET_GEOMETRY, &geometry);

		if (error == 0) {
			st.st_size = geometry.head_count * geometry.cylinder_count
					* geometry.sectors_per_track * geometry.bytes_per_sector;
		}
	}

	if (!(S_ISREG(st.st_mode) || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
		_kern_close(fd);
		return -EINVAL;
	}

	if (st.st_size < PAGE_SIZE) {
		_kern_close(fd);
		return -EINVAL;
	}

	// get file descriptor, vnode, and cookie
	file_descriptor *descriptor = get_fd(get_current_io_context(true), fd);

	vnode *node = fd_vnode(descriptor);
	if (node == nullptr) {
		put_fd(descriptor);
		_kern_close(fd);
		return -EINVAL;
	}

	// do the allocations and prepare the swap_file structure
	swap_file *swap = (swap_file*) malloc(sizeof(swap_file));

	if (swap == nullptr) {
		put_fd(descriptor);
		_kern_close(fd);
		return -ENOMEM;
	}

	swap->fd = fd;
	swap->vnode = node;
	swap->cookie = descriptor->cookie;

	put_fd(descriptor);

	uint32_t pageCount = st.st_size >> PAGE_SHIFT;
	swap->bmp = radix_bitmap_create(pageCount);
	if (swap->bmp == nullptr) {
		free(swap);
		_kern_close(fd);
		return -ENOMEM;
	}

	// set slot index and add this file to swap file list
	mutex_lock(&sSwapFileListLock);
	// TODO: Also check whether the swap file is already registered!
	if (sSwapFileList.IsEmpty()) {
		swap->first_slot = 0;
		swap->last_slot = pageCount;
	} else {
		// leave one page gap between two swap files
		swap->first_slot = sSwapFileList.Last()->last_slot + 1;
		swap->last_slot = swap->first_slot + pageCount;
	}
	sSwapFileList.Add(swap);
	sSwapFileCount++;
	mutex_unlock(&sSwapFileListLock);

	mutex_lock(&sAvailSwapSpaceLock);
	sAvailSwapSpace += (off_t) pageCount * PAGE_SIZE;
	mutex_unlock(&sAvailSwapSpaceLock);

	return 0;
}

int swap_file_delete(const char *path) {
	vnode *node = nullptr;
	int status = vfs_get_vnode_from_path(path, true, &node);
	if (status != 0)
		return status;

	MutexLocker locker(sSwapFileListLock);

	swap_file *swapFile = nullptr;
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
			(swapFile = it.Next()) != nullptr;) {
		if (swapFile->vnode == node)
			break;
	}

	vfs_put_vnode(node);

	if (swapFile == nullptr)
		return -EINVAL;

	// if this file is currently used, we can't delete
	// TODO: mark this swap file deleting, and remove it after releasing
	// all the swap space
	if (swapFile->bmp->free_slots < swapFile->last_slot - swapFile->first_slot)
		return -EINVAL;

	sSwapFileList.Remove(swapFile);
	sSwapFileCount--;
	locker.Unlock();

	mutex_lock(&sAvailSwapSpaceLock);
	sAvailSwapSpace -= (off_t) (swapFile->last_slot - swapFile->first_slot)
			* PAGE_SIZE;
	mutex_unlock(&sAvailSwapSpaceLock);

	_kern_close(swapFile->fd);
	radix_bitmap_destroy(swapFile->bmp);
	free(swapFile);

	return 0;
}

void swap_get_info(system_info *info) {
	MutexLocker locker(sSwapFileListLock);
	for (SwapFileList::Iterator it = sSwapFileList.GetIterator();
			swap_file *swapFile = it.Next();) {
		info->max_swap_pages += swapFile->last_slot - swapFile->first_slot;
		info->free_swap_pages += swapFile->bmp->free_slots;
	}
}

void swap_init() {
	tid_t id;

	sSwapBlockCache = create_object_cache("swapblock", sizeof(swap_block),
			sizeof(void*), nullptr, nullptr, nullptr);

	if (sSwapBlockCache == nullptr)
		panic("swap_init(): can't create object cache for swap blocks");

	int error = object_cache_set_minimum_reserve(sSwapBlockCache,
	MIN_SWAP_BLOCK_RESERVE);
	if (error != 0) {
		panic("swap_init(): object_cache_set_minimum_reserve() failed: %d",
				error);
	}

	sSwapHashTable = new (std::nothrow) SwapHashTable();

	if (sSwapHashTable == nullptr)
		panic("swap_init: Can't allocate hash table");

	if (sSwapHashTable->Init(INITIAL_SWAP_HASH_SIZE) < 0)
		panic("swap_init: Can't resize initial hash");

	id = spawn_kernel_thread_etc(swap_hash_resizer, "swap hash resizer",
	K_LOWEST_ACTIVE_PRIORITY, nullptr, 0);

	if (id < 0)
		panic("swap_init: Can't create swap hash resizer");

	resume_thread(id);
}

//! Used by page daemon to free swap space.
bool
swap_free_page_swap_space(struct page* page)
{
	VMAnonymousCache *cache = dynamic_cast<VMAnonymousCache*>(page->Cache());
	if (cache == NULL)
		return false;

	swap_addr_t slotIndex = cache->_SwapBlockGetAddress(page->cache_offset);
	if (slotIndex == SWAP_SLOT_NONE)
		return false;

	swap_slot_dealloc(slotIndex, 1);
	cache->allocated_swap_size -= PAGE_SIZE;
	cache->_SwapBlockFree(page->cache_offset, 1);

	return true;
}
