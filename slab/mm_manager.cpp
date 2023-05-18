
#include <core/debug.h>
#include <mm/mm_manager.h>
#include <mm/mm_allocator.h>
#include <mm/mm_cache.h>
#include <mm/VMAddressSpace.h>
#include <mm/mm_mapper.h>
#include <mm/mm_locking.h>
#include <core/thread.h>
#include <core/condvar.h>
#include <core/machine_desc.h>
#include <trustware/vm.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <mm/mm_low_mem_framework.h>

mutex_t				MemoryManager::sLock = MUTEX_INITIALIZER("mm lock");
rw_lock_t			MemoryManager::sAreaTableLock = RW_LOCK_INITIALIZER("area table");
MemoryAreaTable 	MemoryManager::sAreaTable;
MemoryArea *		MemoryManager::sFreeAreas;
int					MemoryManager::sFreeAreaCount;
MetaChunkList		MemoryManager::sFreeCompleteMetaChunks = STAILQ_HEAD_INITIALIZER(sFreeCompleteMetaChunks);
MetaChunkList		MemoryManager::sFreeShortMetaChunks = STAILQ_HEAD_INITIALIZER(sFreeShortMetaChunks);
MetaChunkList		MemoryManager::sPartialMetaChunksSmall = STAILQ_HEAD_INITIALIZER(sPartialMetaChunksSmall);
MetaChunkList		MemoryManager::sPartialMetaChunksMedium = STAILQ_HEAD_INITIALIZER(sPartialMetaChunksMedium);
bool				MemoryManager::sMaintenanceNeeded;
cond_var_t			MemoryManager::sMaintenanceCond = CONDVAR_INITIALIZER(sMaintenanceCond, "cache maintenance");
MemoryManager::AllocationEntry*	MemoryManager::sAllocationEntryCanWait;
MemoryManager::AllocationEntry*	MemoryManager::sAllocationEntryDontWait;

static inline MetaChunk * MetaChunkListRemoveHead(MetaChunkList * list) {
	MetaChunk * chunk = STAILQ_FIRST(list);
	if(chunk)
		STAILQ_REMOVE_HEAD(list, link);
	return chunk;
}

static void* sAreaTableBuffer[1024];

#ifdef CONFIG_DEBUG
static inline void* fill_block(void* buffer, size_t size, uint32_t pattern)
{
	if (buffer == nullptr)
		return nullptr;

	size &= ~(sizeof(pattern) - 1);
	for (size_t i = 0; i < size / sizeof(pattern); i++)
		((uint32_t*)buffer)[i] = pattern;

	return buffer;
}
#endif

static inline void* fill_allocated_block(void* buffer, [[maybe_unused]] size_t size)
{
#ifdef CONFIG_DEBUG
	return fill_block(buffer, size, 0xcccccccc);
#else
	return buffer;
#endif
}

void MemoryManager::Init()
{
	sAreaTable.Resize(sAreaTableBuffer,	sizeof(sAreaTableBuffer), true, nullptr);
}

void MemoryManager::_PrepareMetaChunk(MetaChunk* metaChunk, size_t chunkSize)
{
	auto * area = metaChunk->GetArea();

	if (metaChunk == area->metaChunks) {
		// the first chunk is shorter
		size_t unusableSize = roundup(SLAB_AREA_STRUCT_OFFSET + kAreaAdminSize, chunkSize);
		metaChunk->chunkBase = area->BaseAddress() + unusableSize;
		metaChunk->totalSize = SLAB_CHUNK_SIZE_LARGE - unusableSize;
	}

	metaChunk->chunkSize = chunkSize;
	metaChunk->chunkCount = metaChunk->totalSize / chunkSize;
	metaChunk->usedChunkCount = 0;

	metaChunk->freeChunks = nullptr;
	for (int32_t i = metaChunk->chunkCount - 1; i >= 0; i--)
		_push(metaChunk->freeChunks, metaChunk->chunks + i);

	metaChunk->firstFreeChunk = 0;
	metaChunk->lastFreeChunk = metaChunk->chunkCount - 1;
}

bool MemoryManager::_GetChunk(MetaChunkList* metaChunkList, size_t chunkSize,
		MetaChunk*& _metaChunk, MemoryChunk*& _chunk)
{
	MetaChunk * metaChunk = metaChunkList ? STAILQ_FIRST(metaChunkList) : nullptr;

	if(!metaChunk) {
		// no partial meta chunk -- maybe there's a free one
		if (chunkSize == SLAB_CHUNK_SIZE_LARGE) {
			metaChunk = MetaChunkListRemoveHead(&sFreeCompleteMetaChunks);
		} else {
			metaChunk = MetaChunkListRemoveHead(&sFreeShortMetaChunks);

			if(!metaChunk) {
				metaChunk = MetaChunkListRemoveHead(&sFreeCompleteMetaChunks);
			}

			if(metaChunk) {
				STAILQ_INSERT_TAIL(metaChunkList, metaChunk, link);
			}
		}

		if(!metaChunk)
			return false;

		metaChunk->GetArea()->usedMetaChunkCount++;
		_PrepareMetaChunk(metaChunk, chunkSize);
	}

	// allocate the chunk
	if (++metaChunk->usedChunkCount == metaChunk->chunkCount) {
		// meta chunk is full now -- remove it from its list
		if (metaChunkList != nullptr) {
			STAILQ_REMOVE(metaChunkList, metaChunk, MetaChunk, link);
		}
	}

	_chunk = _pop(metaChunk->freeChunks);
	_metaChunk = metaChunk;

	_chunk->reference = 1;

	// update the free range
	uint32_t chunkIndex = _chunk - metaChunk->chunks;
	if (chunkIndex >= metaChunk->firstFreeChunk
			&& chunkIndex <= metaChunk->lastFreeChunk) {
		if (chunkIndex - metaChunk->firstFreeChunk
				<= metaChunk->lastFreeChunk - chunkIndex) {
			metaChunk->firstFreeChunk = chunkIndex + 1;
		} else
			metaChunk->lastFreeChunk = chunkIndex - 1;
	}

	return true;
}

void MemoryManager::_FreeArea(MemoryArea* area, bool areaRemoved, uint32_t flags)
{
	ASSERT(area->usedMetaChunkCount == 0);

	if (!areaRemoved) {
		// remove the area's meta chunks from the free lists
		ASSERT(area->metaChunks[0].usedChunkCount == 0);

		STAILQ_REMOVE(&sFreeShortMetaChunks, &area->metaChunks[0], MetaChunk, link);

		for (int32_t i = 1; i < SLAB_META_CHUNKS_PER_AREA; i++) {
			ASSERT(area->metaChunks[i].usedChunkCount == 0);
			STAILQ_REMOVE(&sFreeCompleteMetaChunks, &area->metaChunks[i], MetaChunk, link);
		}

		// remove the area from the hash table
		WriteLocker writeLocker(sAreaTableLock);
		sAreaTable.RemoveUnchecked(area);
	}

	// We want to keep one or two free areas as a reserve.
	if (sFreeAreaCount <= 1) {
		_PushFreeArea(area);
		return;
	}

	if (area->vmArea == nullptr || (flags & CACHE_DONT_LOCK_KERNEL)) {
		// This is either early in the boot process or we aren't allowed to
		// delete the area now.
		_PushFreeArea(area);
		_RequestMaintenance();
		return;
	}

	mutex_unlock(&sLock);

	size_t memoryToUnreserve = area->reserved_memory_for_mapping;
	delete_area(area->vmArea->id);
	vm_unreserve_memory(memoryToUnreserve);

	mutex_lock(&sLock);
}

void MemoryManager::_FreeChunk(MemoryArea* area, MetaChunk* metaChunk, MemoryChunk* chunk,
				uintptr_t chunkAddress, bool alreadyUnmapped, uint32_t flags)
{
	if (!alreadyUnmapped) {
		mutex_unlock(&sLock);
		_UnmapChunk(area->vmArea, chunkAddress, metaChunk->chunkSize);
		mutex_lock(&sLock);
	}

	_push(metaChunk->freeChunks, chunk);

	uint32_t chunkIndex = chunk - metaChunk->chunks;
	ASSERT(metaChunk->usedChunkCount > 0);

	if (--metaChunk->usedChunkCount == 0) {
		// remove from partial meta chunk list
		if (metaChunk->chunkSize == SLAB_CHUNK_SIZE_SMALL)
			STAILQ_REMOVE(&sPartialMetaChunksSmall, metaChunk, MetaChunk, link);
		else if (metaChunk->chunkSize == SLAB_CHUNK_SIZE_MEDIUM) {
			if(metaChunk->chunkCount > 1) {
				STAILQ_REMOVE(&sPartialMetaChunksMedium, metaChunk, MetaChunk, link);
			}
		}

		// mark empty
		metaChunk->chunkSize = 0;

		// add to free list
		if (metaChunk == area->metaChunks)
			STAILQ_INSERT_HEAD(&sFreeShortMetaChunks, metaChunk, link);
		else
			STAILQ_INSERT_HEAD(&sFreeCompleteMetaChunks, metaChunk, link);

		// free the area, if it is unused now
		ASSERT(area->usedMetaChunkCount > 0);
		if (--area->usedMetaChunkCount == 0) {
			_FreeArea(area, false, flags);
		}
	} else if (metaChunk->usedChunkCount == metaChunk->chunkCount - 1) {
		// the meta chunk was full before -- add it back to its partial chunk
		// list
		if (metaChunk->chunkSize == SLAB_CHUNK_SIZE_SMALL)
			STAILQ_INSERT_HEAD(&sPartialMetaChunksSmall, metaChunk, link);
		else if (metaChunk->chunkSize == SLAB_CHUNK_SIZE_MEDIUM)
			STAILQ_INSERT_HEAD(&sPartialMetaChunksMedium, metaChunk, link);

		metaChunk->firstFreeChunk = chunkIndex;
		metaChunk->lastFreeChunk = chunkIndex;
	} else {
		// extend the free range, if the chunk adjoins
		if (chunkIndex + 1 == metaChunk->firstFreeChunk) {
			uint32_t firstFree = chunkIndex;
			for (; firstFree > 0; firstFree--) {
				MemoryChunk* previousChunk = &metaChunk->chunks[firstFree - 1];
				if (!_IsChunkFree(metaChunk, previousChunk))
					break;
			}
			metaChunk->firstFreeChunk = firstFree;
		} else if (chunkIndex == (uint32_t)metaChunk->lastFreeChunk + 1) {
			uint32_t lastFree = chunkIndex;
			for (; lastFree + 1 < metaChunk->chunkCount; lastFree++) {
				MemoryChunk* nextChunk = &metaChunk->chunks[lastFree + 1];
				if (!_IsChunkFree(metaChunk, nextChunk))
					break;
			}
			metaChunk->lastFreeChunk = lastFree;
		}
	}

}

int MemoryManager::_UnmapChunk(VMArea * vmArea, uintptr_t address, size_t size)
{
	if (vmArea == nullptr)
		return -EINVAL;

	VMTranslationMap* translationMap = VMAddressSpace::Kernel()->TranslationMap();
	VMCache* cache = vm_area_get_locked_cache(vmArea);

	// unmap the pages
	translationMap->Lock();
	translationMap->Unmap(address, size);
	__atomic_fetch_sub(&gMappedPagesCount, size / PAGE_SIZE, __ATOMIC_RELAXED);
	translationMap->Unlock();

	// free the pages
	off_t areaPageOffset = (address - vmArea->Base()) / PAGE_SIZE;
	off_t areaPageEndOffset = areaPageOffset + size / PAGE_SIZE;
	VMCachePagesTree::Iterator it = cache->pages.GetIterator(
		areaPageOffset, true, true);
	while (struct page * page = it.Next()) {
		if (page->cache_offset >= areaPageEndOffset)
			break;
		page->DecrementWiredCount();
		cache->RemovePage(page);
		vm_page_free_etc(cache, page, nullptr);
	}
	cache->ReleaseRefAndUnlock();
	vm_unreserve_memory(size);

	return 0;
}

void MemoryManager::_RequestMaintenance()
{
	if ((sFreeAreaCount > 0 && sFreeAreaCount <= 2) || sMaintenanceNeeded)
		return;

	sMaintenanceNeeded = true;
	sMaintenanceCond.NotifyAll();
}

void MemoryManager::Free(void * pages, uint32_t flags)
{
	MemoryArea * area = _AreaForAddress((uintptr_t)pages);
	MetaChunk* metaChunk = &area->metaChunks[
		((uintptr_t)pages % SLAB_AREA_SIZE) / SLAB_CHUNK_SIZE_LARGE];
	ASSERT(metaChunk->chunkSize > 0);
	ASSERT((uintptr_t)pages >= metaChunk->chunkBase);
	ASSERT(((uintptr_t)pages % metaChunk->chunkSize) == 0);

	// get the chunk
	uint16_t chunkIndex = _ChunkIndexForAddress(metaChunk, (uintptr_t)pages);
	MemoryChunk* chunk = &metaChunk->chunks[chunkIndex];

	ASSERT(chunk->next != nullptr);
	ASSERT(chunk->next < metaChunk->chunks
		|| chunk->next
			>= metaChunk->chunks + SLAB_SMALL_CHUNKS_PER_META_CHUNK);

	MutexLocker locker(sLock);
	_FreeChunk(area, metaChunk, chunk, (uintptr_t)pages, false, flags);
}

bool MemoryManager::_GetChunks(MetaChunkList* metaChunkList, size_t chunkSize,
	uint32_t chunkCount, MetaChunk*& _metaChunk, MemoryChunk*& _chunk)
{
	// the common and less complicated special case
	if (chunkCount == 1)
		return _GetChunk(metaChunkList, chunkSize, _metaChunk, _chunk);

	ASSERT(metaChunkList != nullptr);

	// Iterate through the partial meta chunk list and try to find a free
	// range that is large enough.
	MetaChunk* metaChunk = nullptr;

	STAILQ_FOREACH(metaChunk, metaChunkList, link) {
		if (metaChunk->firstFreeChunk + chunkCount - 1 <= metaChunk->lastFreeChunk) {
			break;
		}
	}

	if (metaChunk == nullptr) {
		// try to get a free meta chunk
		if ((SLAB_CHUNK_SIZE_LARGE - SLAB_AREA_STRUCT_OFFSET - kAreaAdminSize)
				/ chunkSize >= chunkCount)
		{
			metaChunk = MetaChunkListRemoveHead(&sFreeShortMetaChunks);
		}

		if (metaChunk == nullptr) {
			metaChunk = MetaChunkListRemoveHead(&sFreeCompleteMetaChunks);
		}

		if (metaChunk == nullptr)
			return false;

		STAILQ_INSERT_TAIL(metaChunkList, metaChunk, link);
		metaChunk->GetArea()->usedMetaChunkCount++;
		_PrepareMetaChunk(metaChunk, chunkSize);
	}

	// pull the chunks out of the free list
	MemoryChunk* firstChunk = metaChunk->chunks + metaChunk->firstFreeChunk;
	MemoryChunk* lastChunk = firstChunk + (chunkCount - 1);
	MemoryChunk** chunkPointer = &metaChunk->freeChunks;
	uint32_t remainingChunks = chunkCount;
	while (remainingChunks > 0) {
		ASSERT(chunkPointer);
		MemoryChunk* chunk = *chunkPointer;
		if (chunk >= firstChunk && chunk <= lastChunk) {
			*chunkPointer = chunk->next;
			chunk->reference = 1;
			remainingChunks--;
		} else
			chunkPointer = &chunk->next;
	}

	// allocate the chunks
	metaChunk->usedChunkCount += chunkCount;
	if (metaChunk->usedChunkCount == metaChunk->chunkCount) {
		// meta chunk is full now -- remove it from its list
		STAILQ_REMOVE(metaChunkList, metaChunk, MetaChunk, link);
	}

	// update the free range
	metaChunk->firstFreeChunk += chunkCount;

	_chunk = firstChunk;
	_metaChunk = metaChunk;

	return true;
}

void MemoryManager::_AddArea(MemoryArea* area)
{
	// add the area to the hash table
	WriteLocker writeLocker(sAreaTableLock);

	sAreaTable.InsertUnchecked(area);
	writeLocker.Unlock();

	// add the area's meta chunks to the free lists
	STAILQ_INSERT_TAIL(&sFreeShortMetaChunks, &area->metaChunks[0], link);
	for (int32_t i = 1; i < SLAB_META_CHUNKS_PER_AREA; i++) {
		STAILQ_INSERT_TAIL(&sFreeCompleteMetaChunks, &area->metaChunks[i], link);
	}
}

int MemoryManager::_MapChunk(VMArea * vmArea, uintptr_t address, size_t size,
	size_t reserveAdditionalMemory, uint32_t flags)
{
	if (vmArea == nullptr) {
		// everything is mapped anyway
		return 0;
	}

	VMTranslationMap* translationMap = VMAddressSpace::Kernel()->TranslationMap();

	ASSERT(address >= vmArea->Base());
	ASSERT(address + size <= vmArea->End());

	// reserve memory for the chunk
	size_t reservedMemory = size + reserveAdditionalMemory;
	int priority = (flags & CACHE_USE_RESERVE) ? GFP_PRIO_VIP : GFP_PRIO_SYSTEM;
	int error;
	
	if(flags & CACHE_DONT_WAIT_FOR_MEMORY) {
		error = vm_try_reserve_memory(reservedMemory, priority, 0);
	} else if(flags & CACHE_DONT_OOM_KILL) {
		error = vm_try_reserve_memory(reservedMemory, priority, SECONDS(1));
	} else {
		error = vm_try_reserve_memory(reservedMemory, priority, SECONDS(1));

		if(error) {
			// Try OOM killer
			if(!oom_kill_page_alloc_retry(true)) {
				return error;
			}

			error = vm_try_reserve_memory(reservedMemory, priority, SECONDS(1));
		}
	}

	if(error)
		return error;

	// reserve the pages we need now
	size_t reservedPages = size / PAGE_SIZE
			+ translationMap->MaxPagesNeededToMap(address, size);

	vm_reservation_t reservation;

	if (!(flags & CACHE_DONT_WAIT_FOR_MEMORY)) {
		/*
		 * Kernel allocations are restricted, so we can kill
		 * the tasks at will.
		 */

		if (!vm_reserve_pages(&reservation,
				reservedPages, priority, false, !(flags & CACHE_DONT_OOM_KILL))) {
			vm_unreserve_memory(reservedMemory);
			return -ENOMEM;
		}
	} else {
		if (!vm_reserve_pages(&reservation,
				reservedPages, priority, true, false)) {
			vm_unreserve_memory(reservedMemory);
			return -EWOULDBLOCK;
		}
	}

	VMCache* cache = vm_area_get_locked_cache(vmArea);

	// map the pages
	translationMap->Lock();

	size_t areaOffset = address - vmArea->Base();
	size_t endAreaOffset = areaOffset + size;
	for (size_t offset = areaOffset; offset < endAreaOffset;
			offset += PAGE_SIZE) {
		struct page * page = vm_page_allocate_page_etc(&reservation, PAGE_STATE_WIRED);
		cache->InsertPage(page, offset);

		page->IncrementWiredCount();
		__atomic_fetch_add(&gMappedPagesCount, 1, __ATOMIC_RELAXED);

		translationMap->Map(vmArea->Base() + offset,
			page_to_phys(page),
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&reservation);
	}

	translationMap->Unlock();
	cache->ReleaseRefAndUnlock();

	vm_page_unreserve_pages(&reservation);

	return 0;
}

int MemoryManager::_AllocateArea(MemoryArea*& _area, uint32_t flags) {
	mutex_unlock(&sLock);

	size_t pagesNeededToMap = 0;
	vaddr_t areaBase;
	MemoryArea* area;
	VMArea* vmArea = nullptr;

	if (vm_page_allocator_active) {
		int areaID = vm_create_null_area(VMAddressSpace::KernelID(), "kernel heap",
				&areaBase, B_ANY_KERNEL_BLOCK_ADDRESS,
				SLAB_AREA_SIZE, 0);

		if (areaID < 0) {
			mutex_lock(&sLock);
			return areaID;
		}

		area = _AreaForAddress(areaBase);

		VMTranslationMap* translationMap = VMAddressSpace::Kernel()->TranslationMap();

		pagesNeededToMap = translationMap->MaxPagesNeededToMap((uintptr_t) area,
				SLAB_AREA_SIZE);

		vmArea = VMAreaHash::Lookup(areaID);

		int error = _MapChunk(vmArea, (uintptr_t) area, kAreaAdminSize,
				pagesNeededToMap, flags);

		if (error != 0) {
			delete_area(areaID);
			mutex_lock(&sLock);
			return error;
		}
	} else {
		areaBase = vm_allocate_early(SLAB_AREA_SIZE,
		SLAB_AREA_SIZE, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		SLAB_AREA_SIZE);

		if (areaBase == 0) {
			mutex_lock(&sLock);
			return -ENOMEM;
		}

		area = _AreaForAddress((uintptr_t) areaBase);
	}

	// init the area structure
	area->vmArea = vmArea;
	area->reserved_memory_for_mapping = pagesNeededToMap * PAGE_SIZE;
	area->usedMetaChunkCount = 0;
	area->fullyMapped = vmArea == nullptr;

	// init the meta chunks
	for (int32_t i = 0; i < SLAB_META_CHUNKS_PER_AREA; i++) {
		MetaChunk* metaChunk = area->metaChunks + i;
		metaChunk->chunkSize = 0;
		metaChunk->chunkBase = areaBase + i * SLAB_CHUNK_SIZE_LARGE;
		metaChunk->totalSize = SLAB_CHUNK_SIZE_LARGE;
		// Note: chunkBase and totalSize aren't correct for the first
		// meta chunk. They will be set in _PrepareMetaChunk().
		metaChunk->chunkCount = 0;
		metaChunk->usedChunkCount = 0;
		metaChunk->freeChunks = nullptr;
	}

	mutex_lock(&sLock);
	_area = area;

	return 0;
}

int MemoryManager::_AllocateChunks(size_t chunkSize, uint32_t chunkCount,
	uint32_t flags, MetaChunk*& _metaChunk, MemoryChunk*& _chunk)
{
	MetaChunkList* metaChunkList = nullptr;
	if (chunkSize == SLAB_CHUNK_SIZE_SMALL) {
		metaChunkList = &sPartialMetaChunksSmall;
	} else if (chunkSize == SLAB_CHUNK_SIZE_MEDIUM) {
		metaChunkList = &sPartialMetaChunksMedium;
	} else if (chunkSize != SLAB_CHUNK_SIZE_LARGE) {
		panic("Unsupported chunk size: %zd", chunkSize);
	}

	if (_GetChunks(metaChunkList, chunkSize, chunkCount, _metaChunk, _chunk))
		return 0;

	if (sFreeAreas != nullptr) {
		_AddArea(_PopFreeArea());
		_RequestMaintenance();

		return _GetChunks(metaChunkList, chunkSize, chunkCount, _metaChunk, _chunk) ?
				0 : -ENOMEM;
	}

	if ((flags & CACHE_DONT_LOCK_KERNEL) != 0) {
		// We can't create an area with this limitation and we must not wait for
		// someone else doing that.
		return -EWOULDBLOCK;
	}

	// We need to allocate a new area. Wait, if someone else is trying to do
	// the same.
	while (true) {
		AllocationEntry* allocationEntry = nullptr;
		if (sAllocationEntryDontWait != nullptr) {
			allocationEntry = sAllocationEntryDontWait;
		} else if (sAllocationEntryCanWait != nullptr
				&& (flags & CACHE_DONT_WAIT_FOR_MEMORY) == 0)
		{
			allocationEntry = sAllocationEntryCanWait;
		} else
			break;

		DEFINE_CONDVAR_ENTRY(entry);
		allocationEntry->condition.Add(&entry);

		mutex_unlock(&sLock);
		entry.Wait();
		mutex_lock(&sLock);

		if (_GetChunks(metaChunkList, chunkSize, chunkCount, _metaChunk, _chunk)) {
			return 0;
		}
	}

	// prepare the allocation entry others can wait on
	AllocationEntry*& allocationEntry
		= ((flags & CACHE_DONT_WAIT_FOR_MEMORY) == 0)
			? sAllocationEntryCanWait : sAllocationEntryDontWait;

	AllocationEntry myResizeEntry;
	allocationEntry = &myResizeEntry;
	allocationEntry->condition.Init(metaChunkList, "wait for slab area");
	allocationEntry->thread = thread_get_current_thread_id();

	MemoryArea* area;
	int error = _AllocateArea(area, flags);

	allocationEntry->condition.NotifyAll();
	allocationEntry = nullptr;

	if (error != 0)
		return error;

	// Try again to get a meta chunk. Something might have been freed in the
	// meantime. We can free the area in this case.
	if (_GetChunks(metaChunkList, chunkSize, chunkCount, _metaChunk, _chunk)) {
		_FreeArea(area, true, flags);
		return 0;
	}

	_AddArea(area);
	return _GetChunks(metaChunkList, chunkSize, chunkCount, _metaChunk, _chunk) ?
		0 : -ENOMEM;
}

int MemoryManager::AllocateRaw(size_t size, uint32_t flags, void*& _pages)
{
	if (!arch_kernel_allocation_size_valid(size)) {
		// Cap invalid size
		return -ENOMEM;
	}

	size = roundup(size, SLAB_CHUNK_SIZE_SMALL);

	if (size > SLAB_CHUNK_SIZE_LARGE || (flags & CACHE_ALIGN_ON_SIZE) != 0
			|| (size > PAGE_SIZE && (flags & CACHE_CONTIGUOUS_MEMORY) != 0))
	{
		// Requested size greater than a large chunk or an aligned allocation.
		// Allocate as an area.
		if ((flags & CACHE_DONT_LOCK_KERNEL) != 0)
			return -EWOULDBLOCK;

		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address_specification
			= (flags & CACHE_ALIGN_ON_SIZE) != 0
				? B_ANY_KERNEL_BLOCK_ADDRESS : B_RANDOMIZED_ANY_ADDRESS;
		physical_address_restrictions physicalRestrictions = {};

		uint32_t wiring;

		if (size > PAGE_SIZE && (flags & CACHE_CONTIGUOUS_MEMORY) != 0) {
			wiring = B_CONTIGUOUS;
		} else {
			wiring = B_FULL_LOCK;
		}

		area_id area = vm_create_anonymous_area_etc(VMAddressSpace::KernelID(),
				"slab large raw allocation", size, wiring,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				(!(flags & CACHE_DONT_WAIT_FOR_MEMORY) ?
						0 : CREATE_AREA_DONT_WAIT) | CREATE_AREA_DONT_CLEAR, 0,
				&virtualRestrictions, &physicalRestrictions, true,
				(vaddr_t*) &_pages);

		int result = area >= 0 ? 0 : area;

		if (result == 0) {
			if (!(flags & CACHE_CLEAR_MEMORY)) {
				fill_allocated_block(_pages, size);
			} else {
				memset(_pages, 0, size);
			}
		}

		return result;
	}

	// determine chunk size (small or medium)
	size_t chunkSize = SLAB_CHUNK_SIZE_SMALL;
	uint32_t chunkCount = size / SLAB_CHUNK_SIZE_SMALL;

	if (size % SLAB_CHUNK_SIZE_MEDIUM == 0) {
		chunkSize = SLAB_CHUNK_SIZE_MEDIUM;
		chunkCount = size / SLAB_CHUNK_SIZE_MEDIUM;
	}

	MutexLocker locker(sLock);

	// allocate the chunks
	MetaChunk* metaChunk;
	MemoryChunk* chunk;
	int error = _AllocateChunks(chunkSize, chunkCount, flags, metaChunk, chunk);
	if (error != 0)
		return error;

	// map the chunks
	MemoryArea* area = metaChunk->GetArea();
	uintptr_t chunkAddress = _ChunkAddress(metaChunk, chunk);

	locker.Unlock();
	error = _MapChunk(area->vmArea, chunkAddress, size, 0, flags);
	locker.Lock();
	if (error != 0) {
		// something failed -- free the chunks
		for (uint32_t i = 0; i < chunkCount; i++)
			_FreeChunk(area, metaChunk, chunk + i, chunkAddress, true, flags);
		return error;
	}

	chunk->reference = (uintptr_t)chunkAddress + size - 1;
	_pages = (void*)chunkAddress;

	if(!(flags & CACHE_CLEAR_MEMORY))
		fill_allocated_block(_pages, size);
	else
		memset(_pages, 0, size);

	return 0;
}

ObjectCache* MemoryManager::FreeRawOrReturnCache(void* pages, uint32_t flags) {
	// get the area
	uintptr_t areaBase = _AreaBaseAddressForAddress((uintptr_t) pages);

	ReadLocker readLocker(sAreaTableLock);

	MemoryArea* area = sAreaTable.Lookup(areaBase);
	readLocker.Unlock();

	if (area == nullptr) {
		// Probably a large allocation. Look up the VM area.
		AddressSpaceReadLocker spaceLocker(VMAddressSpace::Kernel(), true);
		VMArea *lookupAreaa = VMAddressSpace::Kernel()->LookupArea((uintptr_t)pages);
		spaceLocker.Unlock();

		if (lookupAreaa != nullptr && (uintptr_t) pages == lookupAreaa->Base())
			delete_area(lookupAreaa->id);
		else
			panic("freeing unknown block %p from area %p", pages, lookupAreaa);

		return nullptr;
	}

	MetaChunk* metaChunk = &area->metaChunks[((uintptr_t) pages % SLAB_AREA_SIZE) / SLAB_CHUNK_SIZE_LARGE];

	// get the chunk
	ASSERT(metaChunk->chunkSize > 0);
	ASSERT((uintptr_t )pages >= metaChunk->chunkBase);
	uint16_t chunkIndex = _ChunkIndexForAddress(metaChunk, (uintptr_t) pages);
	MemoryChunk* chunk = &metaChunk->chunks[chunkIndex];

	uintptr_t reference = chunk->reference;
	if ((reference & 1) == 0)
		return (ObjectCache*) reference;

	// Seems we have a raw chunk allocation.
	ASSERT((uintptr_t )pages == _ChunkAddress(metaChunk, chunk));
	ASSERT(reference > (uintptr_t )pages);
	ASSERT(reference <= areaBase + SLAB_AREA_SIZE - 1);
	size_t size = reference - (uintptr_t) pages + 1;
	ASSERT((size % SLAB_CHUNK_SIZE_SMALL) == 0);

	// unmap the chunks
	_UnmapChunk(area->vmArea, (uintptr_t) pages, size);

	// and free them
	MutexLocker locker(sLock);
	uint32_t chunkCount = size / metaChunk->chunkSize;
	for (uint32_t i = 0; i < chunkCount; i++)
		_FreeChunk(area, metaChunk, chunk + i, (uintptr_t) pages, true, flags);

	return nullptr;

}


size_t MemoryManager::AcceptableChunkSize(size_t size)
{
	if (size <= SLAB_CHUNK_SIZE_SMALL)
		return SLAB_CHUNK_SIZE_SMALL;
	if (size <= SLAB_CHUNK_SIZE_MEDIUM)
		return SLAB_CHUNK_SIZE_MEDIUM;
	return SLAB_CHUNK_SIZE_LARGE;
}

int MemoryManager::Allocate(ObjectCache* cache, uint32_t flags, void*& _pages)
{
	size_t chunkSize = cache->slab_size;

	MutexLocker locker(sLock);

	// allocate a chunk
	MetaChunk* metaChunk = nullptr;
	MemoryChunk* chunk = nullptr;
	int error = _AllocateChunks(chunkSize, 1, flags, metaChunk, chunk);
	if (error != 0)
		return error;

	// map the chunk
	MemoryArea* area = metaChunk->GetArea();
	uintptr_t chunkAddress = _ChunkAddress(metaChunk, chunk);

	locker.Unlock();
	error = _MapChunk(area->vmArea, chunkAddress, chunkSize, 0, flags);
	locker.Lock();
	if (error != 0) {
		// something failed -- free the chunk
		_FreeChunk(area, metaChunk, chunk, chunkAddress, true, flags);
		return error;
	}

	chunk->reference = (uintptr_t)cache;
	_pages = (void*)chunkAddress;

	return 0;
}

void memory_manager_init(void)
{
	MemoryManager::Init();
}

void memory_manager_init_post_area(void)
{
	MemoryManager::InitPostArea();
}

void MemoryManager::PerformMaintenance()
{
	MutexLocker locker(sLock);

	while (sMaintenanceNeeded) {
		sMaintenanceNeeded = false;

		// We want to keep one or two areas as a reserve. This way we have at
		// least one area to use in situations when we aren't allowed to
		// allocate one and also avoid ping-pong effects.
		if (sFreeAreaCount > 0 && sFreeAreaCount <= 2)
			return;

		if (sFreeAreaCount == 0) {
			// try to allocate one
			MemoryArea* area;
			if (_AllocateArea(area, CACHE_DONT_OOM_KILL) != 0)
				return;

			_PushFreeArea(area);
			if (sFreeAreaCount > 2)
				sMaintenanceNeeded = true;
		} else {
			// free until we only have two free ones
			while (sFreeAreaCount > 2)
				_FreeArea(_PopFreeArea(), true, 0);

			if (sFreeAreaCount == 0)
				sMaintenanceNeeded = true;
		}
	}
}

ObjectCache* MemoryManager::GetAllocationInfo(void* address, size_t& _size)
{
	// get the area
	ReadLocker readLocker(sAreaTableLock);

	MemoryArea* area = sAreaTable.Lookup(_AreaBaseAddressForAddress((uintptr_t)address));
	readLocker.Unlock();

	if (area == nullptr) {
		AddressSpaceReadLocker locker(VMAddressSpace::Kernel(), true);
		VMArea *allocationArea = VMAddressSpace::Kernel()->LookupArea((uintptr_t)address);
		if (allocationArea != nullptr && (uintptr_t)address == allocationArea->Base())
			_size = allocationArea->Size();
		else
			_size = 0;

		return nullptr;
	}

	MetaChunk* metaChunk = &area->metaChunks[
		((uintptr_t)address % SLAB_AREA_SIZE) / SLAB_CHUNK_SIZE_LARGE];

	// get the chunk
	ASSERT(metaChunk->chunkSize > 0);
	ASSERT((uintptr_t)address >= metaChunk->chunkBase);
	uint16_t chunkIndex = _ChunkIndexForAddress(metaChunk, (uintptr_t)address);

	uintptr_t reference = metaChunk->chunks[chunkIndex].reference;
	if ((reference & 1) == 0) {
		ObjectCache* cache = (ObjectCache*)reference;
		_size = cache->object_size;
		return cache;
	}

	_size = reference - (uintptr_t)address + 1;
	return nullptr;
}

void MemoryManager::_ConvertEarlyArea(MemoryArea* area)
{
	void * address = (void *)area->BaseAddress();
	int areaID = create_area("kernel heap", &address, B_EXACT_ADDRESS,
		SLAB_AREA_SIZE, B_ALREADY_WIRED,
		VMA_TYPE_S_DATA);

	if (areaID < 0)
		panic("out of memory");

	area->vmArea = VMAreaHash::Lookup(areaID);
}

void MemoryManager::_UnmapFreeChunksEarly(MemoryArea* area)
{
	if (!area->fullyMapped)
		return;

	// unmap the space before the Area structure
#if SLAB_AREA_STRUCT_OFFSET > 0
	_UnmapChunk(area->vmArea, area->BaseAddress(), SLAB_AREA_STRUCT_OFFSET);
#endif

	for (int32_t i = 0; i < SLAB_META_CHUNKS_PER_AREA; i++) {
		MetaChunk* metaChunk = area->metaChunks + i;
		if (metaChunk->chunkSize == 0) {
			// meta chunk is free -- unmap it completely
			if (i == 0) {
				_UnmapChunk(area->vmArea, (uintptr_t)area + kAreaAdminSize,
					SLAB_CHUNK_SIZE_LARGE - kAreaAdminSize);
			} else {
				_UnmapChunk(area->vmArea,
					area->BaseAddress() + i * SLAB_CHUNK_SIZE_LARGE,
					SLAB_CHUNK_SIZE_LARGE);
			}
		} else {
			// unmap free chunks
			for (MemoryChunk* chunk = metaChunk->freeChunks; chunk != nullptr;
					chunk = chunk->next) {
				_UnmapChunk(area->vmArea, _ChunkAddress(metaChunk, chunk),
					metaChunk->chunkSize);
			}

			// The first meta chunk might have space before its first chunk.
			if (i == 0) {
				uintptr_t unusedStart = (uintptr_t)area + kAreaAdminSize;
				if (unusedStart < metaChunk->chunkBase) {
					_UnmapChunk(area->vmArea, unusedStart,
						metaChunk->chunkBase - unusedStart);
				}
			}
		}
	}

	area->fullyMapped = false;
}

void MemoryManager::InitPostArea()
{
	// Convert all areas to actual areas. This loop might look a bit weird, but
	// is necessary since creating the actual area involves memory allocations,
	// which in turn can change the situation.
	bool done;
	do {
		done = true;

		for (auto it = sAreaTable.GetIterator();
				MemoryArea* area = it.Next();) {
			if (area->vmArea == nullptr) {
				_ConvertEarlyArea(area);
				done = false;
				break;
			}
		}
	} while (!done);

	// unmap and free unused pages
	if (sFreeAreas != nullptr) {
		// Just "leak" all but the first of the free areas -- the VM will
		// automatically free all unclaimed memory.
		sFreeAreas->next = nullptr;
		sFreeAreaCount = 1;

		MemoryArea* area = sFreeAreas;
		_ConvertEarlyArea(area);
		_UnmapFreeChunksEarly(area);
	}

	for (auto it = sAreaTable.GetIterator();
			MemoryArea* area = it.Next();) {
		_UnmapFreeChunksEarly(area);
	}

	sMaintenanceNeeded = true;

	mm_perform_maintenance_thread_work();

#if defined(CONFIG_KDEBUGGER)
	add_debugger_command_etc("slab_area", &_DumpArea,
		"Dump information on a given slab area",
		"[ -c ] <area>\n"
		"Dump information on a given slab area specified by its base "
			"address.\n"
		"If \"-c\" is given, the chunks of all meta chunks area printed as "
			"well.\n", 0);
	add_debugger_command_etc("slab_areas", &_DumpAreas,
		"List all slab areas",
		"\n"
		"Lists all slab areas.\n", 0);
	add_debugger_command_etc("slab_meta_chunk", &_DumpMetaChunk,
		"Dump information on a given slab meta chunk",
		"<meta chunk>\n"
		"Dump information on a given slab meta chunk specified by its base "
			"or object address.\n", 0);
	add_debugger_command_etc("slab_meta_chunks", &_DumpMetaChunks,
		"List all non-full slab meta chunks",
		"[ -c ]\n"
		"Lists all non-full slab meta chunks.\n"
		"If \"-c\" is given, the chunks of all meta chunks area printed as "
			"well.\n", 0);
	add_debugger_command_etc("slab_raw_allocations", &_DumpRawAllocations,
		"List all raw allocations in slab areas",
		"\n"
		"Lists all raw allocations in slab areas.\n", 0);
#endif
}


#if defined(CONFIG_KDEBUGGER)

/*static*/ int
MemoryManager::_DumpRawAllocations(int, char**)
{
	kprintf("%-*s    meta chunk  chunk  %-*s    size (KB)\n",
		B_PRINTF_POINTER_WIDTH, "area", B_PRINTF_POINTER_WIDTH, "base");

	size_t totalSize = 0;

	for (auto it = sAreaTable.GetIterator();
			auto* area = it.Next();) {
		for (int32_t i = 0; i < SLAB_META_CHUNKS_PER_AREA; i++) {
			MetaChunk* metaChunk = area->metaChunks + i;
			if (metaChunk->chunkSize == 0)
				continue;
			for (uint32_t k = 0; k < metaChunk->chunkCount; k++) {
				auto chunk = metaChunk->chunks + k;

				// skip free chunks
				if (_IsChunkFree(metaChunk, chunk))
					continue;

				uintptr_t reference = chunk->reference;
				if ((reference & 1) == 0 || reference == 1)
					continue;

				uintptr_t chunkAddress = _ChunkAddress(metaChunk, chunk);
				size_t size = reference - chunkAddress + 1;
				totalSize += size;

				kprintf("%p  %10" PRId32 "  %5" PRIu32 "  %p  %9zu"
					"\n", area, i, k, (void*)chunkAddress,
					size / size_t(1024));
			}
		}
	}

	kprintf("total:%*s%9zu\n", (2 * B_PRINTF_POINTER_WIDTH) + 21,
		"", totalSize / 1024);

	return 0;
}


/*static*/ void
MemoryManager::_PrintMetaChunkTableHeader(bool printChunks)
{
	if (printChunks)
		kprintf("chunk        base       cache  object size  cache name\n");
	else
		kprintf("chunk        base\n");
}

/*static*/ void
MemoryManager::_DumpMetaChunk(MetaChunk* metaChunk, bool printChunks,
	bool printHeader)
{
	if (printHeader)
		_PrintMetaChunkTableHeader(printChunks);

	const char* type = "empty";
	if (metaChunk->chunkSize != 0) {
		switch (metaChunk->chunkSize) {
			case SLAB_CHUNK_SIZE_SMALL:
				type = "small";
				break;
			case SLAB_CHUNK_SIZE_MEDIUM:
				type = "medium";
				break;
			case SLAB_CHUNK_SIZE_LARGE:
				type = "large";
				break;
		}
	}

	int metaChunkIndex = metaChunk - metaChunk->GetArea()->metaChunks;
	kprintf("%5d  %p  --- %6s meta chunk", metaChunkIndex,
		(void*)metaChunk->chunkBase, type);
	if (metaChunk->chunkSize != 0) {
		kprintf(": %4u/%4u used, %-4u-%4u free ------------\n",
			metaChunk->usedChunkCount, metaChunk->chunkCount,
			metaChunk->firstFreeChunk, metaChunk->lastFreeChunk);
	} else
		kprintf(" --------------------------------------------\n");

	if (metaChunk->chunkSize == 0 || !printChunks)
		return;

	for (uint32_t i = 0; i < metaChunk->chunkCount; i++) {
		auto chunk = metaChunk->chunks + i;

		// skip free chunks
		if (_IsChunkFree(metaChunk, chunk)) {
			if (!_IsChunkInFreeList(metaChunk, chunk)) {
				kprintf("%5" PRIu32 "  %p  appears free, but isn't in free "
					"list!\n", i, (void*)_ChunkAddress(metaChunk, chunk));
			}

			continue;
		}

		uintptr_t reference = chunk->reference;
		if ((reference & 1) == 0) {
			ObjectCache* cache = (ObjectCache*)reference;
			kprintf("%5" PRIu32 "  %p  %p  %11zu  %s\n", i,
				(void*)_ChunkAddress(metaChunk, chunk), cache,
				cache != nullptr ? cache->object_size : 0,
				cache != nullptr ? cache->name : "");
		} else if (reference != 1) {
			kprintf("%5" PRIu32 "  %p  raw allocation up to %p\n", i,
				(void*)_ChunkAddress(metaChunk, chunk), (void*)reference);
		}
	}
}


/*static*/ int
MemoryManager::_DumpMetaChunk(int argc, char** argv)
{
	if (argc != 2) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	uint64_t address;
	if (!evaluate_debug_expression(argv[1], &address, false))
		return 0;

	auto area = _AreaForAddress(address);

	MetaChunk* metaChunk;
	if ((uintptr_t)address >= (uintptr_t)area->metaChunks
		&& (uintptr_t)address
			< (uintptr_t)(area->metaChunks + SLAB_META_CHUNKS_PER_AREA)) {
		metaChunk = (MetaChunk*)(uintptr_t)address;
	} else {
		metaChunk = area->metaChunks
			+ (address % SLAB_AREA_SIZE) / SLAB_CHUNK_SIZE_LARGE;
	}

	_DumpMetaChunk(metaChunk, true, true);

	return 0;
}


/*static*/ void
MemoryManager::_DumpMetaChunks(const char* name, MetaChunkList& metaChunkList,
	bool printChunks)
{
	kprintf("%s:\n", name);

	MetaChunk * metaChunk;

	STAILQ_FOREACH(metaChunk, &metaChunkList, link) {
		_DumpMetaChunk(metaChunk, printChunks, false);
	}
}


/*static*/ int
MemoryManager::_DumpMetaChunks(int argc, char** argv)
{
	bool printChunks = argc > 1 && strcmp(argv[1], "-c") == 0;

	_PrintMetaChunkTableHeader(printChunks);
	_DumpMetaChunks("free complete", sFreeCompleteMetaChunks, printChunks);
	_DumpMetaChunks("free short", sFreeShortMetaChunks, printChunks);
	_DumpMetaChunks("partial small", sPartialMetaChunksSmall, printChunks);
	_DumpMetaChunks("partial medium", sPartialMetaChunksMedium, printChunks);

	return 0;
}


/*static*/ int
MemoryManager::_DumpArea(int argc, char** argv)
{
	bool printChunks = false;

	int argi = 1;
	while (argi < argc) {
		if (argv[argi][0] != '-')
			break;
		const char* arg = argv[argi++];
		if (strcmp(arg, "-c") == 0) {
			printChunks = true;
		} else {
			print_debugger_command_usage(argv[0]);
			return 0;
		}
	}

	if (argi + 1 != argc) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	uint64_t address;
	if (!evaluate_debug_expression(argv[argi], &address, false))
		return 0;

	auto area = _AreaForAddress((uintptr_t)address);

	for (auto k = 0; k < SLAB_META_CHUNKS_PER_AREA; k++) {
		MetaChunk* metaChunk = area->metaChunks + k;
		_DumpMetaChunk(metaChunk, printChunks, k == 0);
	}

	return 0;
}


/*static*/ int
MemoryManager::_DumpAreas(int, char**)
{
	kprintf("  %*s    %*s   meta      small   medium  large\n",
		B_PRINTF_POINTER_WIDTH, "base", B_PRINTF_POINTER_WIDTH, "area");

	size_t totalTotalSmall = 0;
	size_t totalUsedSmall = 0;
	size_t totalTotalMedium = 0;
	size_t totalUsedMedium = 0;
	size_t totalUsedLarge = 0;
	uint32_t areaCount = 0;

	for (auto it = sAreaTable.GetIterator();
			auto area = it.Next();) {
		areaCount++;

		// sum up the free/used counts for the chunk sizes
		int totalSmall = 0;
		int usedSmall = 0;
		int totalMedium = 0;
		int usedMedium = 0;
		int usedLarge = 0;

		for (int32_t i = 0; i < SLAB_META_CHUNKS_PER_AREA; i++) {
			auto metaChunk = area->metaChunks + i;
			if (metaChunk->chunkSize == 0)
				continue;

			switch (metaChunk->chunkSize) {
				case SLAB_CHUNK_SIZE_SMALL:
					totalSmall += metaChunk->chunkCount;
					usedSmall += metaChunk->usedChunkCount;
					break;
				case SLAB_CHUNK_SIZE_MEDIUM:
					totalMedium += metaChunk->chunkCount;
					usedMedium += metaChunk->usedChunkCount;
					break;
				case SLAB_CHUNK_SIZE_LARGE:
					usedLarge += metaChunk->usedChunkCount;
					break;
			}
		}

		kprintf("%p  %p  %2u/%2u  %4d/%4d  %3d/%3d  %5d\n",
			area, area->vmArea, area->usedMetaChunkCount,
			SLAB_META_CHUNKS_PER_AREA, usedSmall, totalSmall, usedMedium,
			totalMedium, usedLarge);

		totalTotalSmall += totalSmall;
		totalUsedSmall += usedSmall;
		totalTotalMedium += totalMedium;
		totalUsedMedium += usedMedium;
		totalUsedLarge += usedLarge;
	}

	kprintf("%d free area%s:\n", sFreeAreaCount,
		sFreeAreaCount == 1 ? "" : "s");
	for (auto area = sFreeAreas; area != nullptr; area = area->next) {
		areaCount++;
		kprintf("%p  %p\n", area, area->vmArea);
	}

	kprintf("total usage:\n");
	kprintf("  small:    %zu/%zu\n", totalUsedSmall,
		totalTotalSmall);
	kprintf("  medium:   %zu/%zu\n", totalUsedMedium,
		totalTotalMedium);
	kprintf("  large:    %zu\n", totalUsedLarge);
	kprintf("  memory:   %zu/%" PRIu32 " KB\n",
		(totalUsedSmall * SLAB_CHUNK_SIZE_SMALL
			+ totalUsedMedium * SLAB_CHUNK_SIZE_MEDIUM
			+ totalUsedLarge * SLAB_CHUNK_SIZE_LARGE) / 1024,
		areaCount * SLAB_AREA_SIZE / 1024);
	kprintf("  overhead: %zu KB\n",
		areaCount * kAreaAdminSize / 1024);

	return 0;
}

/*static*/ bool
MemoryManager::_IsChunkInFreeList(const MetaChunk* metaChunk,
	const MemoryChunk* chunk)
{
	MemoryChunk* freeChunk = metaChunk->freeChunks;
	while (freeChunk != nullptr) {
		if (freeChunk == chunk)
			return true;
		freeChunk = freeChunk->next;
	}

	return false;
}
#endif
