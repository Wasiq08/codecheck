

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
#include <mm/VMAddressSpace.h>
#include "mm_anonymous_cache.h"
#include <memory>

static rw_lock_t sCacheListLock{"VMCache list"};
static rw_lock_t sAreaCacheLock{"VMArea caches"};

ObjectCache* gCacheRefObjectCache;
ObjectCache* gVMCacheObjectCache;

using all_vm_caches_t = std::aligned_union_t<sizeof(VMCache),
		VMAnonymousNoSwapCache,
		VMAnonymousCache,
		VMVnodeCache,
		VMNullCache,
		VMNonSecureCache,
		VMDeviceCache>;

struct VMCache* vm_cache_acquire_locked_page_cache(struct page * page, bool dontWait)
{
	rw_lock_read_lock(&sCacheListLock);

	while (dontWait) {
		VMCacheRef* cacheRef = page->CacheRef();

		if (cacheRef == nullptr) {
			rw_lock_read_unlock(&sCacheListLock);
			return nullptr;
		}

		VMCache* cache = cacheRef->cache;

		if (!cache->TryLock()) {
			rw_lock_read_unlock(&sCacheListLock);
			return nullptr;
		}

		if (cacheRef == page->CacheRef()) {
			rw_lock_read_unlock(&sCacheListLock);
			cache->AcquireRefLocked();
			return cache;
		}

		// the cache changed in the meantime
		cache->Unlock();
	}

	while (true) {
		VMCacheRef* cacheRef = page->CacheRef();
		if (cacheRef == nullptr) {
			rw_lock_read_unlock(&sCacheListLock);
			return nullptr;
		}

		VMCache* cache = cacheRef->cache;
		if (!cache->SwitchFromReadLock(&sCacheListLock)) {
			// cache has been deleted
			rw_lock_read_lock(&sCacheListLock);
			continue;
		}

		rw_lock_read_lock(&sCacheListLock);
		if (cache == page->Cache()) {
			rw_lock_read_unlock(&sCacheListLock);
			cache->AcquireRefLocked();
			return cache;
		}

		// the cache changed in the meantime
		cache->Unlock();
	}
}

VMCache::~VMCache()
{
	if(cache_ref) {
		object_cache_delete(gCacheRefObjectCache, cache_ref);
	}
}

int VMCache::Init(uint32_t cacheType, int allocationFlags)
{
	type = cacheType;
	cache_ref = new(gCacheRefObjectCache, allocationFlags) VMCacheRef{this,1};

	if(!cache_ref)
		return -ENOMEM;

	return 0;
}

bool VMCache::_IsMergeable() const
{
	return areas == nullptr && temporary && !consumers.IsEmpty() && consumers.Head() == consumers.Tail() &&
	       type != CACHE_TYPE_VNODE &&
	       mergeable &&
	       page_event_waiters.empty();
}

void VMCache::Delete()
{
	if (areas != nullptr)
		panic("cache %p to be deleted still has areas", this);
	if (!consumers.IsEmpty())
		panic("cache %p to be deleted still has consumers", this);

	// free all of the pages in the cache
	while (struct page* page = pages.Root()) {
		if (!page->mappings.IsEmpty() || page->WiredCount() != 0) {
			panic("remove page %p from cache %p: page still has mappings!", page, this);
		}
		// remove it
		pages.Remove(page);
		page->SetCacheRef(nullptr);
		vm_page_free_etc(this, page, nullptr);
	}

	// remove the ref to the source
	if (source) {
		source->_RemoveConsumer(this);
	}

	// We lock and unlock the sCacheListLock, even if the DEBUG_CACHE_LIST is
	// not enabled. This synchronization point is needed for
	// vm_cache_acquire_locked_page_cache().
	rw_lock_write_lock(&sCacheListLock);
	lock.Destroy();
	rw_lock_write_unlock(&sCacheListLock);

	DeleteObject();
}

void VMCache::Unlock(bool consumerLocked)
{
	while (ref_count == 1 && _IsMergeable()) {
		VMCache* consumer = consumers.Head();
		if (consumerLocked) {
			_MergeWithOnlyConsumer();
		} else if (consumer->TryLock()) {
			_MergeWithOnlyConsumer();
			consumer->Unlock();
		} else {
			// Someone else has locked the consumer ATM. Unlock this cache and
			// wait for the consumer lock. Increment the cache's ref count
			// temporarily, so that no one else will try what we are doing or
			// delete the cache.
			ref_count++;
			bool consumerLockedTemp = consumer->SwitchLock(&lock);
			Lock();
			ref_count--;

			if (consumerLockedTemp) {
				if (ref_count == 1 && _IsMergeable()
						&& consumer == consumers.Head()) {
					// nothing has changed in the meantime -- merge
					_MergeWithOnlyConsumer();
				}

				consumer->Unlock();
			}
		}
	}

	if (ref_count == 0) {
		// delete this cache
		Delete();
	} else
		lock.Unlock();
}

struct page * VMCache::LookupPage(off_t offset)
{
	AssertLocked();

	struct page * page = pages.Lookup(offset >> PAGE_SHIFT);

	if (page != nullptr && page->Cache() != this)
		panic("page %p not in cache %p\n", page, this);

	return page;
}


void VMCache::InsertPage(struct page * page, off_t offset)
{
	AssertLocked();

	if (page->CacheRef() != nullptr) {
		panic("insert page %p into cache %p: page cache is set to %p\n",
			page, this, page->Cache());
	}

	page->cache_offset = offset >> PAGE_SHIFT;
	page_count++;
	page->SetCacheRef(cache_ref);

#if defined(CONFIG_DEBUG)
	struct page* otherPage = pages.Lookup(page->cache_offset);
	if (otherPage != nullptr) {
		panic("VMCache::InsertPage(): there's already page %p with cache "
			"offset %#" B_PRIxPHYSADDR " in cache %p; inserting page %p",
			otherPage, (vm_paddr_t)page->cache_offset, this, page);
	}
#endif

	pages.Insert(page);
	virtual_last_page = MAX(virtual_last_page >> PAGE_SHIFT, offset);

	if (page->WiredCount() > 0)
		IncrementWiredPagesCount();
}

void VMCache::RemovePage(struct page * page)
{
	AssertLocked();

	if (page->Cache() != this) {
		panic("remove page %p from cache %p: page cache is set to %p\n", page,
			this, page->Cache());
	}

	pages.Remove(page);
	page_count--;
	page->SetCacheRef(nullptr);

	if(virtual_last_page == page->cache_offset) {
		struct page * maxPage = pages.FindMax();
		if(maxPage)
			virtual_last_page = maxPage->cache_offset;
		else
			virtual_last_page = 0;
	}

	if (page->WiredCount() > 0)
		DecrementWiredPagesCount();
}

/*!	Moves the given page from its current cache inserts it into this cache
	at the given offset.
	Both caches must be locked.
*/
void VMCache::MovePage(struct page * page, off_t offset)
{
	VMCache* oldCache = page->Cache();

	AssertLocked();
	oldCache->AssertLocked();

	// remove from old cache
	oldCache->pages.Remove(page);
	oldCache->page_count--;

	// change the offset
	page->cache_offset = offset >> PAGE_SHIFT;

	// insert here
	pages.Insert(page);
	page_count++;
	page->SetCacheRef(cache_ref);

	virtual_last_page = MAX(virtual_last_page >> PAGE_SHIFT, offset);

	if (page->WiredCount() > 0) {
		IncrementWiredPagesCount();
		oldCache->DecrementWiredPagesCount();
	}
}

/*!	Moves the given page from its current cache inserts it into this cache.
	Both caches must be locked.
*/
void VMCache::MovePage(struct page * page)
{
	MovePage(page, page->cache_offset << PAGE_SHIFT);
}

/*!	Moves all pages from the given cache to this one.
	Both caches must be locked. This cache must be empty.
*/
void VMCache::MoveAllPages(VMCache* fromCache)
{
	AssertLocked();
	fromCache->AssertLocked();
	ASSERT(page_count == 0);

	fromCache->pages.swap(pages);
	page_count = fromCache->page_count;
	fromCache->page_count = 0;
	wired_pages_count = fromCache->wired_pages_count;
	fromCache->wired_pages_count = 0;

	// swap the VMCacheRefs
	rw_lock_write_lock(&sCacheListLock);
	std::swap(cache_ref, fromCache->cache_ref);
	cache_ref->cache = this;
	fromCache->cache_ref->cache = fromCache;
	rw_lock_write_unlock(&sCacheListLock);
}

struct page * VMCache::FirstPage(off_t offset, bool greater, bool orEqual)
{
	return pages.FindClosest(offset, greater, orEqual);
}

/*!	Makes this case the source of the \a consumer cache,
	and adds the \a consumer to its list.
	This also grabs a reference to the source cache.
	Assumes you have the cache and the consumer's lock held.
*/
void VMCache::AddConsumer(VMCache* consumer)
{
	AssertLocked();
	consumer->AssertLocked();

	consumer->source = this;
	consumers.Add(consumer);

	AcquireRefLocked();
	AcquireStoreRef();
}

/*!	Adds the \a area to this cache.
	Assumes you have the locked the cache.
*/
void VMCache::InsertAreaLocked(VMArea* area)
{
	AssertLocked();

	area->cache_next = areas;
	if (area->cache_next)
		area->cache_next->cache_prev = area;
	area->cache_prev = nullptr;
	areas = area;

	AcquireStoreRef();
}

void VMCache::RemoveArea(VMArea* area)
{
	// We release the store reference first, since otherwise we would reverse
	// the locking order or even deadlock ourselves (... -> free_vnode() -> ...
	// -> bfs_remove_vnode() -> ... -> file_cache_set_size() -> mutex_lock()).
	// Also cf. _RemoveConsumer().
	ReleaseStoreRef();

	AutoLocker<VMCache> locker(this);

	if (area->cache_prev)
		area->cache_prev->cache_next = area->cache_next;
	if (area->cache_next)
		area->cache_next->cache_prev = area->cache_prev;
	if (areas == area)
		areas = area->cache_next;
}

/*!	Transfers the areas from \a fromCache to this cache. This cache must not
	have areas yet. Both caches must be locked.
*/
void VMCache::TransferAreas(VMCache* fromCache)
{
	rw_lock_write_lock(&sAreaCacheLock);
	AssertLocked();
	fromCache->AssertLocked();
	ASSERT(areas == nullptr);

	areas = fromCache->areas;
	fromCache->areas = nullptr;

	for (VMArea* area = areas; area != nullptr; area = area->cache_next) {
		area->cache = this;
		AcquireRefLocked();
		fromCache->ReleaseRefLocked();
	}
	rw_lock_write_unlock(&sAreaCacheLock);
}

uint32_t VMCache::CountWritableAreas(VMArea* ignoreArea) const
{
	uint32_t count = 0;

	for (VMArea* area = areas; area != nullptr; area = area->cache_next) {
		if (area != ignoreArea
			&& (area->protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0) {
			count++;
		}
	}

	return count;
}

int VMCache::WriteModified()
{
	if (temporary)
		return 0;

	Lock();
	int status = vm_page_write_modified_pages(this);
	Unlock();

	return status;
}

int VMCache::SetMinimalCommitment(off_t commitment, int priority)
{
	AssertLocked();

	int status = 0;

	// If we don't have enough committed space to cover through to the new end
	// of the area...
	if (committed_size < commitment) {
		// ToDo: should we check if the cache's virtual size is large
		//	enough for a commitment of that size?

		// try to commit more memory
		status = Commit(commitment, priority);
	}

	return status;
}

/*!	This function updates the size field of the cache.
	If needed, it will free up all pages that don't belong to the cache anymore.
	The cache lock must be held when you call it.
	Since removed pages don't belong to the cache any longer, they are not
	written back before they will be removed.

	Note, this function may temporarily release the cache lock in case it
	has to wait for busy pages.
*/
int VMCache::Resize(off_t newSize, int priority)
{
	this->AssertLocked();

	int status = Commit(newSize - virtual_base, priority);
	if (status != 0)
		return status;

	vm_pindex_t oldPageCount = (vm_pindex_t)((virtual_end + PAGE_SIZE - 1)
		>> PAGE_SHIFT);
	vm_pindex_t newPageCount = (vm_pindex_t)((newSize + PAGE_SIZE - 1)
		>> PAGE_SHIFT);

	if (newPageCount < oldPageCount) {
		// we need to remove all pages in the cache outside of the new virtual
		// size
		while (_FreePageRange(pages.GetIterator(newPageCount, true, true)))
			;
	}

	virtual_end = newSize;
	return 0;
}

/*!	This function updates the virtual_base field of the cache.
	If needed, it will free up all pages that don't belong to the cache anymore.
	The cache lock must be held when you call it.
	Since removed pages don't belong to the cache any longer, they are not
	written back before they will be removed.

	Note, this function may temporarily release the cache lock in case it
	has to wait for busy pages.
*/
int
VMCache::Rebase(off_t newBase, int priority)
{
	this->AssertLocked();

	int status = Commit(virtual_end - newBase, priority);
	if (status != 0)
		return status;

	vm_pindex_t basePage = (vm_pindex_t)(newBase >> PAGE_SHIFT);

	if (newBase > virtual_base) {
		// we need to remove all pages in the cache outside of the new virtual
		// base
		while (_FreePageRange(pages.GetIterator(), &basePage))
			;
	}

	virtual_base = newBase;
	return 0;
}



/*!	Moves pages in the given range from the source cache into this cache. Both
	caches must be locked.
*/
int
VMCache::Adopt(VMCache* sourceToAdopt, off_t offset, off_t size, off_t newOffset)
{
	off_t startPage = offset >> PAGE_SHIFT;
	off_t endPage = (offset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	off_t offsetChange = newOffset - offset;

	VMCachePagesTree::Iterator it = sourceToAdopt->pages.GetIterator(startPage, true,
	                                                                 true);
	for (struct page* page = it.Next();
				page != NULL && page->cache_offset < endPage;
				page = it.Next()) {
		MovePage(page, (page->cache_offset << PAGE_SHIFT) + offsetChange);
	}

	return 0;
}

/*!	You have to call this function with the VMCache lock held. */
int VMCache::FlushAndRemoveAllPages()
{
	while (page_count > 0) {
		// write back modified pages
		int status = vm_page_write_modified_pages(this);
		if (status != 0)
			return status;

		// remove pages
		for (VMCachePagesTree::Iterator it = pages.GetIterator();
				struct page * page = it.Next();)
		{
			if (page->busy) {
				// wait for page to become unbusy
				WaitForPageEvents(page, PAGE_EVENT_NOT_BUSY, true);

				// restart from the start of the list
				it = pages.GetIterator();
				continue;
			}

			// skip modified pages -- they will be written back in the next
			// iteration
			if (page->state == PAGE_STATE_MODIFIED)
				continue;

			// We can't remove mapped pages.
			if (page->IsMapped())
				return -EBUSY;

			RemovePage(page);
			vm_page_free_etc(this, page, nullptr);
		}
	}

	return 0;
}

int VMCache::Commit(off_t size, int)
{
	committed_size = size;
	return 0;
}

bool VMCache::HasPage(off_t)
{
	// In accordance with Fault() the default implementation doesn't have a
	// backing store and doesn't allow faults.
	return false;
}

off_t VMCache::Read(off_t, const generic_iovec *, uint32_t, size_t,
		size_t  *)
{
	return -EIO;
}

off_t VMCache::Write(off_t, const generic_iovec *, uint32_t, size_t,
		size_t  *)
{
	return -EIO;
}

int VMCache::WriteAsync(off_t offset, const struct generic_iovec* vecs, size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* callback)
{
	// Not supported, fall back to the synchronous hook.
	size_t transferred = numBytes;
	int error = Write(offset, vecs, flags, count, &transferred);

	if (callback != nullptr)
		callback->IOFinished(error, transferred != numBytes, transferred);

	return error;
}

bool VMCache::CanWritePage(off_t)
{
	return false;
}

int VMCache::Fault(struct VMAddressSpace *, off_t)
{
	return -EFAULT;
}


void VMCache::Merge(VMCache* sourceToMerge)
{
	for (VMCachePagesTree::Iterator it = sourceToMerge->pages.GetIterator();
			struct page * page = it.Next();) {
		// Note: Removing the current node while iterating through a
		// IteratableSplayTree is safe.
		struct page * consumerPage = LookupPage((off_t)page->cache_offset << PAGE_SHIFT);
		if (consumerPage == nullptr) {
			// the page is not yet in the consumer cache - move it upwards
			MovePage(page);
		}
	}
}

int VMCache::AcquireUnreferencedStoreRef()
{
	return 0;
}

void VMCache::AcquireStoreRef()
{
}

void VMCache::ReleaseStoreRef()
{
}

void VMCache::_MergeWithOnlyConsumer()
{
	VMCache* consumer = consumers.RemoveHead();

	// merge the cache
	consumer->Merge(this);

	// The remaining consumer has got a new source.
	if (source != nullptr) {
		VMCache* newSource = source;

		newSource->Lock();

		newSource->consumers.Remove(this);
		newSource->consumers.Add(consumer);
		consumer->source = newSource;
		source = nullptr;

		newSource->Unlock();
	} else
		consumer->source = nullptr;

	// Release the reference the cache's consumer owned. The consumer takes
	// over the cache's ref to its source (if any) instead.
	ReleaseRefLocked();
}

void
VMCache::_RemoveConsumer(VMCache* consumer)
{
	consumer->AssertLocked();

	// Remove the store ref before locking the cache. Otherwise we'd call into
	// the VFS while holding the cache lock, which would reverse the usual
	// locking order.
	ReleaseStoreRef();

	// remove the consumer from the cache, but keep its reference until later
	Lock();
	consumers.Remove(consumer);
	consumer->source = nullptr;

	ReleaseRefAndUnlock();
}


/*!	Waits until one or more events happened for a given page which belongs to
	this cache.
	The cache must be locked. It will be unlocked by the method. \a relock
	specifies whether the method shall re-lock the cache before returning.
	\param page The page for which to wait.
	\param events The mask of events the caller is interested in.
	\param relock If \c true, the cache will be locked when returning,
		otherwise it won't be locked.
*/
void VMCache::WaitForPageEvents(struct page * page, uint32_t events, bool relock)
{
	PageEventWaiter waiter(page, events);

	page_event_waiters.push_back(waiter);

	Unlock();

	waiter.Wait();

	if (relock) {
		Lock();
	}
}

void VMCache::_NotifyPageEvents(struct page * page, uint32_t events)
{
	page_event_waiters.remove_and_dispose_if(
			[page, events](const PageEventWaiter &waiter) -> bool {
				return waiter.get_page() == page && (waiter.get_events() & events) != 0;
			},
			[](PageEventWaiter *waiter) {
				waiter->WakeUp();
			});
}

bool VMCache::_FreePageRange(VMCachePagesTree::Iterator it, vm_pindex_t* toPage)
{
	for (struct page* page = it.Next();
		page != NULL && (toPage == nullptr || page->cache_offset < (off_t)*toPage);
		page = it.Next()) {

		if (page->busy) {
			if (page->busy_writing) {
				// We cannot wait for the page to become available
				// as we might cause a deadlock this way
				page->busy_writing = false;
					// this will notify the writer to free the page
				continue;
			}

			// wait for page to become unbusy
			WaitForPageEvents(page, PAGE_EVENT_NOT_BUSY, true);
			return true;
		}

		// remove the page and put it into the free queue
		vm_remove_all_page_mappings(page);
		ASSERT(page->WiredCount() == 0);
			// TODO: Find a real solution! If the page is wired
			// temporarily (e.g. by lock_memory()), we actually must not
			// unmap it!
		RemovePage(page);
			// Note: When iterating through a IteratableSplayTree
			// removing the current node is safe.

		vm_page_free_etc(this, page, nullptr);
	}

	return false;
}

void VMCache::MarkPageUnbusy(struct page * page)
{
	ASSERT(page->busy);
	page->busy = 0;
	NotifyPageEvents(page, PAGE_EVENT_NOT_BUSY);
}

struct VMCache* vm_area_get_locked_cache(struct VMArea * area)
{
	rw_lock_read_lock(&sAreaCacheLock);

	while (true) {
		VMCache* cache = area->cache;

		if (!cache) {
			rw_lock_read_unlock(&sAreaCacheLock);
			return nullptr;
		}

		if (!cache->SwitchFromReadLock(&sAreaCacheLock)) {
			// cache has been deleted
			rw_lock_read_lock(&sAreaCacheLock);
			continue;
		}

		rw_lock_read_lock(&sAreaCacheLock);

		if (cache == area->cache) {
			cache->AcquireRefLocked();
			rw_lock_read_unlock(&sAreaCacheLock);
			return cache;
		}

		// the cache changed in the meantime
		cache->Unlock();
	}
}

void vm_area_put_locked_cache(struct VMCache* cache)
{
	cache->ReleaseRefAndUnlock();
}

VMAnonymousNoSwapCache::~VMAnonymousNoSwapCache()
{
	vm_unreserve_memory(committed_size);
}

int VMAnonymousNoSwapCache::Init(bool canOvercommit, int32_t numPrecommittedPages,
	int32_t numGuardPages, int allocationFlags)
{
	int error = VMCache::Init(CACHE_TYPE_RAM, allocationFlags);
	if (error != 0)
		return error;

	fCanOvercommit = canOvercommit;
	fHasPrecommitted = false;
	fPrecommittedPages = std::min<int>(numPrecommittedPages, 255);
	fGuardedSize = numGuardPages * PAGE_SIZE;

	return 0;
}

int VMAnonymousNoSwapCache::Commit(off_t size, int priority)
{
	// If we can overcommit, we don't commit here, but in Fault(). We always
	// unreserve memory, if we're asked to shrink our commitment, though.
	if (fCanOvercommit && size > committed_size) {
		if (fHasPrecommitted) {
			return 0;
		}

		// pre-commit some pages to make a later failure less probable
		fHasPrecommitted = true;
		off_t precommitted = fPrecommittedPages * PAGE_SIZE;
		if (size > precommitted)
			size = precommitted;
	}

	// Check to see how much we could commit - we need real memory

	if (size > committed_size) {
		// try to commit
		if(vm_try_reserve_memory(size - committed_size, priority, SECONDS(1)) != 0) {
			return -ENOMEM;
		}
	} else {
		// we can release some
		vm_unreserve_memory(committed_size - size);
	}

	committed_size = size;
	return 0;
}


bool VMAnonymousNoSwapCache::HasPage(off_t)
{
	return false;
}

int VMAnonymousNoSwapCache::Fault(struct VMAddressSpace* aspace, off_t offset)
{
	if (fGuardedSize > 0) {
		off_t guardOffset;

		guardOffset = 0;

		// report stack fault, guard page hit!
		if (offset >= guardOffset && offset < guardOffset + fGuardedSize) {
			return -EFAULT;
		}
	}

	if (fCanOvercommit) {
		if (fPrecommittedPages == 0) {
			// never commit more than needed
			if (committed_size / PAGE_SIZE > (off_t)page_count)
				return VM_FAULT_DONT_HANDLE;

			// try to commit additional memory
			int priority = (aspace == VMAddressSpace::Kernel())
				? GFP_PRIO_SYSTEM : GFP_PRIO_USER;

			if (vm_try_reserve_memory(PAGE_SIZE, priority, 0) < 0) {
				static struct timeval log_time;
				static int log_pps;

				if (ppsratecheck(&log_time, &log_pps, 1)) {
					printk(KERN_ERR,
							"VMAnonymousNoSwapCache(%p): Failed to reserve %zd bytes of RAM.\n",
							this, (size_t)PAGE_SIZE);
				}

				return -ENOMEM;
			}

			committed_size += PAGE_SIZE;
		} else {
			fPrecommittedPages--;
		}
	}

	return VM_FAULT_DONT_HANDLE;
}

void VMAnonymousNoSwapCache::DeleteObject()
{
	object_cache_delete(gVMCacheObjectCache, this);
}

int VMCacheFactory::CreateAnonymousCache(VMCache*& _cache, bool canOvercommit,
	int32_t numPrecommittedPages, int32_t numGuardPages,
		bool swappable, int, uint32_t allocationFlags) {
	if (swappable) {
		VMAnonymousCache *cache =
				new (gVMCacheObjectCache, allocationFlags) VMAnonymousCache();

		if (!cache) {
			return -ENOMEM;
		}

		int error = cache->Init(canOvercommit, numPrecommittedPages, numGuardPages,
				allocationFlags);

		if (error != 0) {
			cache->Delete();
			return error;
		}

		_cache = cache;
	} else {
		VMAnonymousNoSwapCache* cache = new (gVMCacheObjectCache,
				allocationFlags) VMAnonymousNoSwapCache();

		if (!cache) {
			return -ENOMEM;
		}

		int error = cache->Init(canOvercommit, numPrecommittedPages,
				numGuardPages, allocationFlags);

		if (error != 0) {
			cache->Delete();
			return error;
		}

		_cache = cache;
	}

	return 0;
}

template<typename Cache> Cache * CreateCacheObjectT() {
	Cache * object = new(gVMCacheObjectCache, uint32_t(0)) Cache();
	ASSERT(object != nullptr);
	int error = object->Init(0);
	if(error < 0)
		panic("Can't initialize cache object");
	return object;
}

int VMCacheFactory::CreateDeviceCache(VMCache *&_cache, uint32_t) {
	static VMDeviceCache *cache = CreateCacheObjectT<VMDeviceCache>();
	cache->AcquireRef();
	_cache = cache;
	return 0;
}

int VMCacheFactory::CreateNullCache(int, VMCache*& _cache, uint32_t allocationFlags) {
	VMNullCache * cache = new(gVMCacheObjectCache, allocationFlags) VMNullCache();

	if(!cache)
		return -ENOMEM;

	int error = cache->Init(allocationFlags);
	if (error != 0) {
		cache->Delete();
		return error;
	}

	_cache = cache;
	return 0;
}

int VMCacheFactory::CreateNonSecureCache(VMCache*& _cache, uint32_t) {
	static VMNonSecureCache * cache = CreateCacheObjectT<VMNonSecureCache>();
	cache->AcquireRef();
	_cache = cache;
	return 0;
}

int VMCacheFactory::CreateVnodeCache(VMCache*& _cache, struct vnode* vnode, uint32_t allocationFlags)
{
	VMVnodeCache * cache = new(gVMCacheObjectCache, allocationFlags) VMVnodeCache();

	if(!cache)
		return -ENOMEM;

	int error = cache->Init(vnode, allocationFlags);
	if (error != 0) {
		cache->Delete();
		return error;
	}

	_cache = cache;
	return 0;
}

int VMDeviceCache::Init(int allocationFlags) {
	this->mergeable = false;
	return VMCache::Init(CACHE_TYPE_DEVICE, allocationFlags);
}

void VMDeviceCache::DeleteObject() {
	object_cache_delete(gVMCacheObjectCache, this);
}

int VMNullCache::Init(int allocationFlags) {
	this->mergeable = false;
	return VMCache::Init(CACHE_TYPE_NULL, allocationFlags);
}

void VMNullCache::DeleteObject() {
	object_cache_delete(gVMCacheObjectCache, this);
}

int VMNonSecureCache::Init(int allocationFlags) {
	this->mergeable = false;
	return VMCache::Init(CACHE_TYPE_DEVICE, allocationFlags);
}

void VMNonSecureCache::DeleteObject() {
	object_cache_delete(gVMCacheObjectCache, this);
}

int VMVnodeCache::Init(struct vnode * vnode, int allocationFlags)
{
	int error = VMCache::Init(CACHE_TYPE_VNODE, allocationFlags);
	if (error != 0)
		return error;

	fVnode = vnode;
	fVnodeDeleted = false;

	vfs_vnode_to_node_ref(fVnode, &fDevice, &fInode);
	this->mergeable = false;

	return 0;
}

bool VMVnodeCache::HasPage(off_t offset)
{
	return roundup2(offset, PAGE_SIZE) >= virtual_base && offset < virtual_end;
}

bool VMVnodeCache::CanWritePage(off_t)
{
	if(temporary)
		return false;

	return true;
}

off_t VMVnodeCache::Read(off_t offset, const generic_iovec * vecs, uint32_t flags, size_t count,
			size_t  *_numBytes)
{
	size_t bytesUntouched = *_numBytes;

	int status = vfs_read_pages(fVnode, nullptr, offset, vecs, count, flags, _numBytes);

	size_t bytesEnd = *_numBytes;

	if (offset + (off_t)bytesEnd > virtual_end)
		bytesEnd = virtual_end - offset;

	// If the request could be filled completely, or an error occured,
	// we're done here
	if (status != 0 || bytesUntouched == bytesEnd)
		return status;

	bytesUntouched -= bytesEnd;

	// Clear out any leftovers that were not touched by the above read - we're
	// doing this here so that not every file system/device has to implement
	// this
	for (int32_t i = count; i-- > 0 && bytesUntouched != 0;) {
		size_t length = MIN(bytesUntouched, vecs[i].length);

		uint64_t address = vecs[i].base + vecs[i].length - length;
		if ((flags & GENERIC_IO_USE_PHYSICAL) != 0)
			vm_memset_physical(address, 0, length);
		else
			memset((void*)(uintptr_t)address, 0, length);

		bytesUntouched -= length;
	}

	return 0;
}

off_t VMVnodeCache::Write(off_t offset, const generic_iovec * vecs, uint32_t flags, size_t count,
			size_t  *_numBytes)
{
	return vfs_write_pages(fVnode, nullptr, offset, vecs, count, flags, _numBytes);
}

int VMVnodeCache::WriteAsync(off_t offset, const struct generic_iovec* vecs, size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* callback)
{
	return vfs_asynchronous_write_pages(fVnode, nullptr, offset, vecs, count,
			numBytes, flags, callback);
}

int VMVnodeCache::Fault(struct VMAddressSpace *, off_t offset)
{
	if (!HasPage(offset))
		return -EFAULT;

	// vm_soft_fault() reads the page in.
	return VM_FAULT_DONT_HANDLE;
}

int VMVnodeCache::AcquireUnreferencedStoreRef()
{
	// Quick check whether getting a vnode reference is still allowed. Only
	// after a successful vfs_get_vnode() the check is safe (since then we've
	// either got the reference to our vnode, or have been notified that it is
	// toast), but the check is cheap and saves quite a bit of work in case the
	// condition holds.
	if (fVnodeDeleted)
		return -EBUSY;

	struct vnode* vnode;
	int status = vfs_get_vnode(fDevice, fInode, false, &vnode);

	// If successful, update the store's vnode pointer, so that release_ref()
	// won't use a stale pointer.
	if (status == 0 && fVnodeDeleted) {
		vfs_put_vnode(vnode);
		status = -EBUSY;
	}

	return status;
}

void VMVnodeCache::AcquireStoreRef()
{
	vfs_acquire_vnode(fVnode);
}

void VMVnodeCache::ReleaseStoreRef()
{
	vfs_put_vnode(fVnode);
}

void VMVnodeCache::DeleteObject()
{
	object_cache_delete(gVMCacheObjectCache, this);
}

void VMCache::MarkDeletedForVFS(VMCache * cache)
{
	if(cache->type == CACHE_TYPE_VNODE) {
		static_cast<VMVnodeCache *>(cache)->fVnodeDeleted = true;
	}
}


/*! Discards pages in the given range. */
int
VMCache::Discard(off_t offset, off_t size)
{
	vm_pindex_t startPage = offset >> PAGE_SHIFT;
	vm_pindex_t endPage = (offset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	while (_FreePageRange(pages.GetIterator(startPage, true, true), &endPage))
		;

	return 0;
}

extern "C" void vm_cache_init(void)
{
	// Create object caches for the structures we allocate here.
	gCacheRefObjectCache = create_object_cache("cache refs", sizeof(VMCacheRef),
		0, nullptr, nullptr, nullptr);
	gVMCacheObjectCache = create_object_cache(
		"vm caches", sizeof(all_vm_caches_t), alignof(all_vm_caches_t),
		nullptr, nullptr, nullptr);

	if (gCacheRefObjectCache == nullptr
			|| gVMCacheObjectCache == nullptr)
	{
		panic("vm_cache_init(): Failed to create object caches!");
	}
}


#if defined(CONFIG_KDEBUGGER)
const char* vm_cache_type_to_string(int32_t type)
{
	switch (type) {
		case CACHE_TYPE_RAM:
			return "RAM";
		case CACHE_TYPE_DEVICE:
			return "device";
		case CACHE_TYPE_VNODE:
			return "vnode";
		case CACHE_TYPE_NULL:
			return "null";
		case CACHE_TYPE_NONSECURE:
			return "nonsecure";
		case CACHE_TYPE_NONSECURE_DEVICE:
			return "nonsecure_device";
		case CACHE_TYPE_PFNMAP:
			return "pfnmap";
		case CACHE_TYPE_NONSECURE_PFNMAP:
			return "nonsecure_pfnmap";
		default:
			return "unknown";
	}
}

void VMCache::Dump(bool showPages) const
{
	kprintf("CACHE %p:\n", this);
	kprintf("  ref_count:    %" PRId32 "\n", RefCount());
	kprintf("  source:       %p\n", source);
	kprintf("  type:         %s\n", vm_cache_type_to_string(type));
	kprintf("  virtual_base: 0x%" PRIx64 "\n", (uint64_t)virtual_base);
	kprintf("  virtual_end:  0x%" PRIx64 "\n", (uint64_t)virtual_end);
	kprintf("  temporary:    %" PRIu32 "\n", temporary);
	kprintf("  lock:         %p\n", &lock);
	kprintf("  areas:\n");

	for (VMArea * area = areas; area != nullptr; area = area->cache_next) {
		kprintf("    area 0x%" PRIx32 ", %s\n", area->id, area->name);
		kprintf("\tbase_addr:  0x%" PRIxPTR ", size: 0x%" PRIxPTR "\n", area->Base(),
			area->Size());
		kprintf("\tprotection: 0x%" PRIx32 "\n", area->protection);
		kprintf("\towner:      0x%" PRIx32 "\n", area->address_space->ID());
	}

	kprintf("  consumers:\n");
	for (ConsumerList::ConstIterator it = consumers.GetIterator();
		 	VMCache* consumer = it.Next();) {
		kprintf("\t%p\n", consumer);
	}

	kprintf("  pages:\n");
	if (showPages) {
		for (auto it = pages.GetIterator();
				auto * page = it.Next();) {
			if (page->state != PAGE_STATE_UNUSED) {
				kprintf("\t%p ppn %#" B_PRIxPHYSADDR " offset %zd"
					" state %u (%s) wired_count %u\n", page,
					page_to_phys(page), (size_t)page->cache_offset,
					page->State(), page_state_to_string(page->State()),
					page->WiredCount());
			} else {
				kprintf("\t%p DUMMY PAGE state %u (%s)\n",
					page, page->State(), page_state_to_string(page->State()));
			}
		}
	} else
		kprintf("\t%" PRIu32 " in cache\n", page_count);
}
#endif
