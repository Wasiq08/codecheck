

#include <core/debug.h>
#include <mm/mm_cache.h>
#include <mm/fs_cache.h>
#include <mm/mm_resource.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>

// maximum number of iovecs per request
#define MAX_IO_VECS			32	// 128 kB
#define MAX_FILE_IO_VECS	32

#define BYPASS_IO_SIZE		65536
#define LAST_ACCESSES		3

struct file_cache_ref {
	VMCache			*cache;
	struct vnode	*vnode;
	off_t			last_access[LAST_ACCESSES];
		// TODO: it would probably be enough to only store the least
		//	significant 31 bits, and make this uint32_t (one bit for
		//	write vs. read)
	int32_t			last_access_index;
	uint16_t		disabled_count;

	inline void SetLastAccess(int32_t index, off_t access, bool isWrite)
	{
		// we remember writes as negative offsets
		last_access[index] = isWrite ? -access : access;
	}

	[[nodiscard]] inline off_t LastAccess(int32_t index, bool isWrite) const
	{
		return isWrite ? -last_access[index] : last_access[index];
	}

	inline uint32_t LastAccessPageOffset(int32_t index, bool isWrite)
	{
		return LastAccess(index, isWrite) >> PAGE_SHIFT;
	}
};

typedef int (*cache_func)(file_cache_ref* ref, void* cookie, off_t offset,
		int32_t pageOffset, uintptr_t buffer, size_t bufferSize, bool useBuffer,
		vm_reservation_t * reservation, size_t reservePages);

static void add_to_iovec(generic_iovec* vecs, uint32_t &index, uint32_t max,
	uint64_t address, size_t size);

static const uint32_t kZeroVecCount = 32;
static const size_t kZeroVecSize = kZeroVecCount * PAGE_SIZE;
static vm_paddr_t sZeroPage;	// physical address
static generic_iovec sZeroVecs[kZeroVecCount];

static inline bool
access_is_sequential(file_cache_ref* ref)
{
	return ref->last_access[ref->last_access_index] != 0;
}

static inline void
push_access(file_cache_ref* ref, off_t offset, size_t bytes,
	bool isWrite)
{
	int32_t index = ref->last_access_index;
	int32_t previous = index - 1;
	if (previous < 0)
		previous = LAST_ACCESSES - 1;

	if (offset != ref->LastAccess(previous, isWrite))
		ref->last_access[previous] = 0;

	ref->SetLastAccess(index, offset + bytes, isWrite);

	if (++index >= LAST_ACCESSES)
		index = 0;
	ref->last_access_index = index;
}

static bool reserve_pages(file_cache_ref* ref, vm_reservation_t* reservation,
	size_t reservePages, bool isWrite)
{
	if (low_resource_state(B_KERNEL_RESOURCE_PAGES) != B_NO_LOW_RESOURCE &&
			ref->cache->type == CACHE_TYPE_VNODE)
	{
		VMCache* cache = ref->cache;
		cache->Lock();

		if (cache->consumers.IsEmpty() && cache->areas == nullptr && access_is_sequential(ref)) {
			// we are not mapped, and we're accessed sequentially
			if (isWrite) {
				// Just write some pages back, and actually wait until they
				// have been written back in order to relieve the page pressure
				// a bit.
				int32_t index = ref->last_access_index;
				int32_t previous = index - 1;
				if (previous < 0)
					previous = LAST_ACCESSES - 1;

				vm_page_write_modified_page_range(cache,
					ref->LastAccessPageOffset(previous, true),
					ref->LastAccessPageOffset(index, true));
			} else {
				// free some pages from our cache
				// TODO: start with oldest
				uint32_t left = reservePages;
				struct page * page;
				for (VMCachePagesTree::Iterator it = cache->pages.GetIterator();
						(page = it.Next()) != nullptr && left > 0;) {
					if (page->state == PAGE_STATE_CACHED && !page->busy) {
						ASSERT(!page->IsMapped());
						ASSERT(!page->modified);
						cache->RemovePage(page);
						vm_page_set_state(page, PAGE_STATE_FREE);
						left--;
					}
				}
			}
		}

		cache->Unlock();
	}

	return vm_reserve_pages(
			reservation,
			reservePages,
			GFP_PRIO_USER,
			false,
			false) != 0;
}

static inline int read_pages_and_clear_partial(file_cache_ref* ref, void* cookie, off_t offset,
		const generic_iovec* vecs, size_t count, uint32_t flags,
		size_t * _numBytes)
{
	size_t bytesUntouched = *_numBytes;

	int status = vfs_read_pages(ref->vnode, cookie, offset, vecs, count, flags, _numBytes);

	size_t bytesEnd = *_numBytes;

	if (offset + (off_t)bytesEnd > ref->cache->virtual_end)
		bytesEnd = ref->cache->virtual_end - offset;

	if (status == 0 && bytesEnd < bytesUntouched) {
		// Clear out any leftovers that were not touched by the above read.
		// We're doing this here so that not every file system/device has to
		// implement this.
		bytesUntouched -= bytesEnd;

		for (int32_t i = count; i-- > 0 && bytesUntouched != 0; ) {
			size_t length = std::min<size_t>(bytesUntouched, vecs[i].length);
			vm_memset_physical(vecs[i].base + vecs[i].length - length, 0, length);
			bytesUntouched -= length;
		}
	}

	return status;
}


static int read_into_cache(file_cache_ref* ref, void* cookie, off_t offset,
		int32_t pageOffset, uintptr_t buffer, size_t bufferSize, bool useBuffer,
		vm_reservation_t* reservation, size_t reservePages)
{
	VMCache* cache = ref->cache;

	// TODO: We're using way too much stack! Rather allocate a sufficiently
	// large chunk on the heap.
	generic_iovec vecs[MAX_IO_VECS];
	uint32_t vecCount = 0;

	size_t numBytes = ROUND_PGUP(pageOffset + bufferSize);
	struct page * pages[MAX_IO_VECS];
	int32_t pageIndex = 0;

	// allocate pages for the cache and mark them busy
	for (size_t pos = 0; pos < numBytes; pos += PAGE_SIZE) {
		struct page* page = pages[pageIndex++] = vm_page_allocate_page_etc(
				reservation, PAGE_STATE_CACHED | VM_PAGE_ALLOC_BUSY);
		cache->InsertPage(page, offset + pos);
		add_to_iovec(vecs, vecCount, MAX_IO_VECS,
				page_to_phys(page), PAGE_SIZE);
		// TODO: check if the array is large enough (currently panics)!
	}

	push_access(ref, offset, bufferSize, false);
	cache->Unlock();
	vm_page_unreserve_pages(reservation);

	// read file into reserved pages
	int status = read_pages_and_clear_partial(ref, cookie, offset, vecs,
			vecCount, GENERIC_IO_USE_PHYSICAL, &numBytes);

	if (status != 0) {
		printk(KERN_ERR, "Failed to read pages into memory (%d)\n", status);

		cache->Lock();

		for (int32_t i = 0; i < pageIndex; i++) {
			cache->NotifyPageEvents(pages[i], PAGE_EVENT_NOT_BUSY);
			cache->RemovePage(pages[i]);
			vm_page_set_state(pages[i], PAGE_STATE_FREE);
		}

		return status;
	}

	// copy the pages if needed and unmap them again

	for (int32_t i = 0; i < pageIndex; i++) {
		if (useBuffer && bufferSize != 0) {
			size_t bytes = MIN(bufferSize, (size_t)PAGE_SIZE - pageOffset);

			vm_memcpy_from_physical((void*)buffer,
				page_to_phys(pages[i]) + pageOffset,
				bytes, (uintptr_t)buffer < VM_KERNEL_SPACE_BASE);

			buffer += bytes;
			bufferSize -= bytes;
			pageOffset = 0;
		}
	}

	if(!reserve_pages(ref, reservation, reservePages, false))  {
		cache->Lock();

		for (int32_t i = 0; i < pageIndex; i++) {
			cache->NotifyPageEvents(pages[i], PAGE_EVENT_NOT_BUSY);
			cache->RemovePage(pages[i]);
			vm_page_set_state(pages[i], PAGE_STATE_FREE);
		}

		return -ENOMEM;
	}

	cache->Lock();

	// make the pages accessible in the cache
	for (int32_t i = pageIndex; i-- > 0;) {
		cache->MarkPageUnbusy(pages[i]);
	}

	return 0;
}

static int read_from_file(file_cache_ref* ref, void* cookie, off_t offset,
		int32_t pageOffset, uintptr_t buffer, size_t bufferSize, bool useBuffer,
		vm_reservation_t* reservation, size_t reservePages)
{
	if (!useBuffer)
		return 0;

	generic_iovec vec;
	vec.base = buffer;
	vec.length = bufferSize;

	push_access(ref, offset, bufferSize, false);
	ref->cache->Unlock();
	vm_page_unreserve_pages(reservation);

	size_t toRead = bufferSize;
	int status = vfs_read_pages(ref->vnode, cookie, offset + pageOffset, &vec, 1, 0, &toRead);

	if (status == 0) {
		if(!reserve_pages(ref, reservation, reservePages, false)) {
			status = -ENOMEM;
		}
	}

	ref->cache->Lock();

	return status;
}

static int write_to_cache(file_cache_ref* ref, void* cookie, off_t offset,
		int32_t pageOffset, uintptr_t buffer, size_t bufferSize, bool useBuffer,
		vm_reservation_t* reservation, size_t reservePages)
{
	// TODO: We're using way too much stack! Rather allocate a sufficiently
	// large chunk on the heap.
	generic_iovec vecs[MAX_IO_VECS];
	uint32_t vecCount = 0;
	size_t numBytes = ROUND_PGUP(pageOffset + bufferSize);
	struct page* pages[MAX_IO_VECS];
	int32_t pageIndex = 0;
	int status = 0;

	// ToDo: this should be settable somewhere
	bool writeThrough = false;

	// allocate pages for the cache and mark them busy
	for (size_t pos = 0; pos < numBytes; pos += PAGE_SIZE) {
		// TODO: if space is becoming tight, and this cache is already grown
		//	big - shouldn't we better steal the pages directly in that case?
		//	(a working set like approach for the file cache)
		// TODO: the pages we allocate here should have been reserved upfront
		//	in cache_io()
		struct page* page = pages[pageIndex++] = vm_page_allocate_page_etc(
			reservation,
			(writeThrough ? PAGE_STATE_CACHED : PAGE_STATE_MODIFIED)
				| VM_PAGE_ALLOC_BUSY);

		page->modified = !writeThrough;

		ref->cache->InsertPage(page, offset + pos);

		add_to_iovec(vecs, vecCount, MAX_IO_VECS, page_to_phys(page), PAGE_SIZE);
	}

	push_access(ref, offset, bufferSize, true);
	ref->cache->Unlock();
	vm_page_unreserve_pages(reservation);

	// copy contents (and read in partially written pages first)

	if (pageOffset != 0) {
		// This is only a partial write, so we have to read the rest of the page
		// from the file to have consistent data in the cache
		generic_iovec readVec = { vecs[0].base, PAGE_SIZE };
		size_t bytesRead = PAGE_SIZE;

		status = vfs_read_pages(ref->vnode, cookie, offset, &readVec, 1, GENERIC_IO_USE_PHYSICAL, &bytesRead);

		if (status < 0) {
			ref->cache->Lock();

			for (int32_t i = pageIndex; i-- > 0;) {
				ref->cache->MarkPageUnbusy(pages[i]);
				ref->cache->RemovePage(pages[i]);
				vm_page_set_state(pages[i], PAGE_STATE_FREE);
			}

			return status;
		}
	}

	size_t lastPageOffset = (pageOffset + bufferSize) % PAGE_SIZE;
	if (lastPageOffset != 0 && vecCount > 0) {
		// get the last page in the I/O vectors
		uint64_t last = (uintptr_t)vecs[vecCount - 1].base
			+ vecs[vecCount - 1].length - PAGE_SIZE;

		if ((off_t)(offset + pageOffset + bufferSize) == ref->cache->virtual_end) {
			// the space in the page after this write action needs to be cleaned
			vm_memset_physical(last + lastPageOffset, 0, PAGE_SIZE - lastPageOffset);
		} else {
			// the end of this write does not happen on a page boundary, so we
			// need to fetch the last page before we can update it
			generic_iovec readVec = { last, PAGE_SIZE };
			size_t bytesRead = PAGE_SIZE;

			status = vfs_read_pages(ref->vnode, cookie, ROUND_PGUP(offset + pageOffset + bufferSize) - PAGE_SIZE,
					&readVec, 1, GENERIC_IO_USE_PHYSICAL, &bytesRead);

			if (status < 0)  {
				ref->cache->Lock();

				for (int32_t i = pageIndex; i-- > 0;) {
					ref->cache->MarkPageUnbusy(pages[i]);
					ref->cache->RemovePage(pages[i]);
					vm_page_set_state(pages[i], PAGE_STATE_FREE);
				}

				return status;
			}

			if (bytesRead < PAGE_SIZE) {
				// the space beyond the file size needs to be cleaned
					vm_memset_physical(last + bytesRead, 0, PAGE_SIZE - bytesRead);
			}
		}
	}

	for (uint32_t i = 0; i < vecCount; i++) {
		uint64_t base = vecs[i].base;
		size_t bytes = MIN((size_t)bufferSize, size_t(vecs[i].length - pageOffset));

		if (useBuffer) {
			// copy data from user buffer
			vm_memcpy_to_physical(base + pageOffset, (void*)buffer, bytes,
				buffer < VM_KERNEL_SPACE_BASE);
		} else {
			// clear buffer instead
			vm_memset_physical(base + pageOffset, 0, bytes);
		}

		bufferSize -= bytes;
		if (bufferSize == 0)
			break;

		buffer += bytes;
		pageOffset = 0;
	}

	if (writeThrough) {
		// write cached pages back to the file if we were asked to do that
		int writeStatus = vfs_write_pages(ref->vnode, cookie, offset, vecs,
				vecCount, GENERIC_IO_USE_PHYSICAL, &numBytes);
		if (writeStatus < 0) {
			ref->cache->Lock();

			for (int32_t i = pageIndex; i-- > 0;) {
				ref->cache->MarkPageUnbusy(pages[i]);
				ref->cache->RemovePage(pages[i]);
				vm_page_set_state(pages[i], PAGE_STATE_FREE);
			}

			return writeStatus;
		}
	}

	if (status == 0) {
		if(!reserve_pages(ref, reservation, reservePages, true)) {
			ref->cache->Lock();

			for (int32_t i = pageIndex; i-- > 0;) {
				ref->cache->MarkPageUnbusy(pages[i]);
				ref->cache->RemovePage(pages[i]);
				vm_page_set_state(pages[i], PAGE_STATE_FREE);
			}

			return -ENOMEM;
		}
	}

	ref->cache->Lock();

	// make the pages accessible in the cache
	for (int32_t i = pageIndex; i-- > 0;) {
		ref->cache->MarkPageUnbusy(pages[i]);
	}

	return status;
}

static int write_to_file(file_cache_ref* ref, void* cookie, off_t offset,
		int32_t pageOffset, uintptr_t buffer, size_t bufferSize, bool useBuffer,
		vm_reservation_t* reservation, size_t reservePages)
{
	push_access(ref, offset, bufferSize, true);
	ref->cache->Unlock();
	vm_page_unreserve_pages(reservation);

	int status = 0;

	if (!useBuffer) {
		while (bufferSize > 0) {
			size_t written = MIN(bufferSize, kZeroVecSize);
			status = vfs_write_pages(ref->vnode, cookie, offset + pageOffset,
					sZeroVecs, kZeroVecCount, GENERIC_IO_USE_PHYSICAL, &written);
			if (status != 0) {
				ref->cache->Lock();
				return status;
			}
			if (written == 0) {
				ref->cache->Lock();
				return -ENXIO;
			}

			bufferSize -= written;
			pageOffset += written;
		}
	} else {
		generic_iovec vec;
		vec.base = buffer;
		vec.length = bufferSize;
		size_t toWrite = bufferSize;
		status = vfs_write_pages(ref->vnode, cookie, offset + pageOffset, &vec,
				1, 0, &toWrite);
	}

	if (status == 0) {
		if(!reserve_pages(ref, reservation, reservePages, true)) {
			status = -ENOMEM;
		}
	}

	ref->cache->Lock();

	return status;
}

static inline int satisfy_cache_io(file_cache_ref* ref, void* cookie, cache_func function,
	off_t offset, uintptr_t buffer, bool useBuffer, int32_t &pageOffset,
	size_t bytesLeft, size_t &reservePages, off_t &lastOffset,
	uintptr_t &lastBuffer, int32_t &lastPageOffset, size_t &lastLeft,
	size_t &lastReservedPages, vm_reservation_t* reservation)
{
	if (lastBuffer == buffer)
		return 0;

	size_t requestSize = buffer - lastBuffer;
	reservePages = MIN(MAX_IO_VECS, (lastLeft - requestSize
		+ lastPageOffset + PAGE_SIZE - 1) >> PAGE_SHIFT);

	int status = function(ref, cookie, lastOffset, lastPageOffset,
		lastBuffer, requestSize, useBuffer, reservation, reservePages);
	if (status == 0) {
		lastReservedPages = reservePages;
		lastBuffer = buffer;
		lastLeft = bytesLeft;
		lastOffset = offset;
		lastPageOffset = 0;
		pageOffset = 0;
	}
	return status;
}

static int cache_io(void* _cacheRef, void* cookie, off_t offset, uintptr_t buffer,
		size_t* _size, bool doWrite)
{
	if (_cacheRef == nullptr)
		panic("cache_io() called with NULL ref!\n");

	file_cache_ref* ref = (file_cache_ref*)_cacheRef;
	VMCache* cache = ref->cache;
	bool useBuffer = buffer != 0;

	int32_t pageOffset = offset & (PAGE_SIZE - 1);
	size_t size = *_size;
	offset -= pageOffset;

	// "offset" and "lastOffset" are always aligned to B_PAGE_SIZE,
	// the "last*" variables always point to the end of the last
	// satisfied request part

	const uint32_t kMaxChunkSize = MAX_IO_VECS * PAGE_SIZE;
	size_t bytesLeft = size, lastLeft = size;
	int32_t lastPageOffset = pageOffset;
	uintptr_t lastBuffer = buffer;
	off_t lastOffset = offset;
	size_t lastReservedPages = MIN(MAX_IO_VECS, (pageOffset + bytesLeft
		+ PAGE_SIZE - 1) >> PAGE_SHIFT);
	size_t reservePages = 0;
	size_t pagesProcessed = 0;
	cache_func function = nullptr;
	vm_reservation_t reservation;

	if(!reserve_pages(ref, &reservation, lastReservedPages, doWrite))
		return -ENOMEM;

	AutoLocker<VMCache> locker(cache);

	while (bytesLeft > 0) {
		// Periodically reevaluate the low memory situation and select the
		// read/write hook accordingly
		if (pagesProcessed % 32 == 0) {
			if (size >= BYPASS_IO_SIZE
				&& low_resource_state(B_KERNEL_RESOURCE_PAGES)
					!= B_NO_LOW_RESOURCE) {
				// In low memory situations we bypass the cache beyond a
				// certain I/O size.
				function = doWrite ? write_to_file : read_from_file;
			} else
				function = doWrite ? write_to_cache : read_into_cache;
		}

		// check if this page is already in memory
		struct page * page = cache->LookupPage(offset);

		if (page != nullptr) {
			// The page may be busy - since we need to unlock the cache sometime
			// in the near future, we need to satisfy the request of the pages
			// we didn't get yet (to make sure no one else interferes in the
			// meantime).
			int status = satisfy_cache_io(ref, cookie, function, offset,
				buffer, useBuffer, pageOffset, bytesLeft, reservePages,
				lastOffset, lastBuffer, lastPageOffset, lastLeft,
				lastReservedPages, &reservation);
			if (status != 0)
				return status;

			// Since satisfy_cache_io() unlocks the cache, we need to look up
			// the page again.
			page = cache->LookupPage(offset);
			if (page != nullptr && page->busy) {
				cache->WaitForPageEvents(page, PAGE_EVENT_NOT_BUSY, true);
				continue;
			}
		}

		size_t bytesInPage = MIN(size_t(PAGE_SIZE - pageOffset), bytesLeft);

		if (page != nullptr) {
			if (doWrite || useBuffer) {
				// Since the following user_mem{cpy,set}() might cause a page
				// fault, which in turn might cause pages to be reserved, we
				// need to unlock the cache temporarily to avoid a potential
				// deadlock. To make sure that our page doesn't go away, we mark
				// it busy for the time.
				page->busy = true;
				locker.Unlock();

				// copy the contents of the page already in memory
				vm_paddr_t pageAddress = page_to_phys(page) + pageOffset;
				bool userBuffer = buffer < VM_KERNEL_SPACE_BASE;
				if (doWrite) {
					if (useBuffer) {
						vm_memcpy_to_physical(pageAddress, (void*)buffer,
							bytesInPage, userBuffer);
					} else {
						vm_memset_physical(pageAddress, 0, bytesInPage);
					}
				} else if (useBuffer) {
					vm_memcpy_from_physical((void*)buffer, pageAddress,
						bytesInPage, userBuffer);
				}

				locker.Lock();

				if (doWrite) {
					page->modified = true;

					if (page->state != PAGE_STATE_MODIFIED)
						vm_page_set_state(page, PAGE_STATE_MODIFIED);
				}

				cache->MarkPageUnbusy(page);
			}

			// If it is cached only, requeue the page, so the respective queue
			// roughly remains LRU first sorted.
			if (page->state == PAGE_STATE_CACHED
					|| page->state == PAGE_STATE_MODIFIED) {
				vm_page_requeue(page, true);
			}

			if (bytesLeft <= bytesInPage) {
				// we've read the last page, so we're done!
				locker.Unlock();
				vm_page_unreserve_pages(&reservation);
				return 0;
			}

			// prepare a potential gap request
			lastBuffer = buffer + bytesInPage;
			lastLeft = bytesLeft - bytesInPage;
			lastOffset = offset + PAGE_SIZE;
			lastPageOffset = 0;
		}

		if (bytesLeft <= bytesInPage)
			break;

		buffer += bytesInPage;
		bytesLeft -= bytesInPage;
		pageOffset = 0;
		offset += PAGE_SIZE;
		pagesProcessed++;

		if (buffer - lastBuffer + lastPageOffset >= kMaxChunkSize) {
			int status = satisfy_cache_io(ref, cookie, function, offset,
				buffer, useBuffer, pageOffset, bytesLeft, reservePages,
				lastOffset, lastBuffer, lastPageOffset, lastLeft,
				lastReservedPages, &reservation);
			if (status != 0)
				return status;
		}
	}

	// fill the last remaining bytes of the request (either write or read)

	return function(ref, cookie, lastOffset, lastPageOffset, lastBuffer,
		lastLeft, useBuffer, &reservation, 0);
}

void file_cache_init(void)
{
	// allocate a clean page we can use for writing zeroes
	vm_reservation_t reservation;
	if(!vm_reserve_pages(&reservation, 1, GFP_PRIO_SYSTEM,
			false, false))
		panic("Can't initialize file cache");
	struct page * page = vm_page_allocate_page_etc(&reservation,
		PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
	ASSERT(page != nullptr);
	vm_page_unreserve_pages(&reservation);

	sZeroPage = page_to_phys(page);

	for (auto & sZeroVec : sZeroVecs) {
		sZeroVec.base = sZeroPage;
		sZeroVec.length = PAGE_SIZE;
	}
}

vm_paddr_t file_cache_empty_zero_page(void)
{
	return sZeroPage;
}

void * file_cache_create(fs_id mountID, vnode_id vnodeID, off_t size, bool temporaryCache)
{
	file_cache_ref* ref = new(std::nothrow) file_cache_ref;

	if (ref == nullptr)
		return nullptr;

	memset(ref->last_access, 0, sizeof(ref->last_access));
	ref->last_access_index = 0;
	ref->disabled_count = 0;

	if (vfs_lookup_vnode(mountID, vnodeID, &ref->vnode) != 0)
		goto err1;

	// Gets (usually creates) the cache for the node
	if (vfs_get_vnode_cache(ref->vnode, &ref->cache, true) != 0)
		goto err1;

	ref->cache->virtual_end = size;

	if(temporaryCache) {
		ref->cache->temporary = true;
	}

	return ref;

err1:
	delete ref;
	return nullptr;
}

void file_cache_delete(void* _cacheRef)
{
	file_cache_ref* ref = (file_cache_ref*)_cacheRef;

	if (ref == nullptr)
		return;

	ref->cache->ReleaseRef();
	delete ref;
}

int file_cache_set_size(void* _cacheRef, off_t newSize)
{
	file_cache_ref* ref = (file_cache_ref*)_cacheRef;

	if (ref == nullptr)
		return 0;

	VMCache* cache = ref->cache;
	AutoLocker<VMCache> _(cache);

	off_t oldSize = cache->virtual_end;
	int status = cache->Resize(newSize, GFP_PRIO_USER);
		// Note, the priority doesn't really matter, since this cache doesn't
		// reserve any memory.
	if (status == 0 && newSize < oldSize) {
		// We may have a new partial page at the end of the cache that must be
		// cleared.
		uint32_t partialBytes = newSize % PAGE_SIZE;
		if (partialBytes != 0) {
			struct page * page = cache->LookupPage(newSize - partialBytes);
			if (page != nullptr) {
				vm_memset_physical(page_to_phys(page) + partialBytes, 0, PAGE_SIZE - partialBytes);
			}
		}
	}

	return status;
}

int file_cache_sync(void* _cacheRef)
{
	file_cache_ref* ref = (file_cache_ref*)_cacheRef;
	if (ref == nullptr)
		return -EINVAL;

	return ref->cache->WriteModified();
}

static void
add_to_iovec(generic_iovec* vecs, uint32_t &index, uint32_t max,
		uint64_t address, size_t size)
{
	if (index > 0 && vecs[index - 1].base + vecs[index - 1].length == address) {
		// the iovec can be combined with the previous one
		vecs[index - 1].length += size;
		return;
	}

	if (index == max)
		panic("no more space for iovecs!");

	// we need to start a new iovec
	vecs[index].base = address;
	vecs[index].length = size;
	index++;
}

int file_cache_read(void* _cacheRef, void * cookie, off_t offset, void* buffer, size_t* _size)
{
	file_cache_ref* ref = (file_cache_ref*)_cacheRef;

	       // Bounds checking. We do this here so it applies to uncached I/O.
	if (offset < 0)
		return -EINVAL;

	const off_t fileSize = ref->cache->virtual_end;
	if (offset >= fileSize || *_size == 0) {
		*_size = 0;
		return 0;
	}

	if ((off_t) (offset + *_size) > fileSize)
		*_size = fileSize - offset;

	if (ref->disabled_count > 0) {
		// Caching is disabled -- read directly from the file.
		generic_iovec vec;
		vec.base = (uintptr_t)buffer;
		vec.length = *_size;
		return vfs_read_pages(ref->vnode, cookie, offset, &vec, 1, 0, _size);
	}

	return cache_io(ref, cookie, offset, (uintptr_t)buffer, _size, false);
}

static int
write_zeros_to_file(struct vnode *vnode, void *cookie, off_t offset,
		size_t *_size) {
	size_t size = *_size;
	int status = 0;
	while (size > 0) {
		size_t length = std::min(size, kZeroVecSize);
		generic_iovec *vecs = sZeroVecs;
		generic_iovec vec;
		size_t count = kZeroVecCount;
		if (length != kZeroVecSize) {
			if (length > PAGE_SIZE) {
				length = trunc_page(length);
				count = length / PAGE_SIZE;
			} else {
				vec.base = sZeroPage;
				vec.length = length;
				vecs = &vec;
				count = 1;
			}
		}

		status = vfs_write_pages(vnode, cookie, offset, vecs, count,
				GENERIC_IO_USE_PHYSICAL, &length);
		if (status != 0 || length == 0)
			break;

		offset += length;
		size -= length;
	}

	*_size = *_size - size;
	return status;
}

int file_cache_write(void *_cacheRef, void *cookie, off_t offset,
		const void *buffer, size_t *_size) {
	file_cache_ref *ref = (file_cache_ref*) _cacheRef;

	if(*_size == 0)
		return 0;
	// We don't do bounds checking here, as we are relying on the
	// file system which called us to already have done that and made
	// adjustments as necessary, unlike in read().

	if (ref->disabled_count > 0) {
		// Caching is disabled -- write directly to the file.

		if (buffer != nullptr) {
			generic_iovec vec;
			vec.base = (uintptr_t) buffer;
			size_t size = vec.length = *_size;

			int error = vfs_write_pages(ref->vnode, cookie, offset, &vec, 1, 0,
					&size);

			*_size = size;
			return error;
		}

		return write_zeros_to_file(ref->vnode, cookie, offset, _size);
	}

	return cache_io(ref, cookie, offset,
			(uintptr_t) const_cast<void*>(buffer), _size, true);
}

void file_cache_enable(void* _cacheRef)
{
	file_cache_ref* ref = (file_cache_ref*)_cacheRef;

	AutoLocker<VMCache> _(ref->cache);

	if (ref->disabled_count == 0) {
		panic("Unbalanced file_cache_enable()!");
	}

	ref->disabled_count--;
}

int file_cache_disable(void* _cacheRef)
{
	// TODO: This function only removes all pages from the cache and prevents
	// that the file cache functions add any new ones until re-enabled. The
	// VM (on page fault) can still add pages, if the file is mmap()ed. We
	// should mark the cache to prevent shared mappings of the file and fix
	// the page fault code to deal correctly with private mappings (i.e. only
	// insert pages in consumer caches).

	file_cache_ref* ref = (file_cache_ref*)_cacheRef;

	AutoLocker<VMCache> _(ref->cache);

	// If already disabled, there's nothing to do for us.
	if (ref->disabled_count > 0) {
		ref->disabled_count++;
		return 0;
	}

	// The file cache is not yet disabled. We need to evict all cached pages.
	int error = ref->cache->FlushAndRemoveAllPages();
	if (error != 0)
		return error;

	ref->disabled_count++;
	return 0;
}
