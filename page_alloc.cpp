
#include <cstdio>
#include <mm/mm.h>
#include <mm/page.h>
#include <core/machine_desc.h>
#include <fs/rfs.h>
#include <mm/oom.h>
#include <core/condvar.h>
#include <core/mutex.h>
#include <core/debug.h>
#include <cinttypes>
#include <mm/mm_resource.h>
#include <mm/mm_low_mem_framework.h>
#include <mm/mm_cache.h>
#include <mm/VMAddressSpace.h>

#include <cstring>

#include <trustware/system_info.h>
#include <trustware/types.h>
#include <trustware/vm.h>

#include <mm/mm_manager.h>
#include <mm/mm_page_queue.h>

#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <trustware/AutoDeleter.h>

#include <mm/mm_mapper.h>

#include <driver/dynamicmem.h>
#include <mm/VMKernelAddressSpace.h>

#include <boost/noncopyable.hpp>

#if defined(CONFIG_KDEBUGGER)
#include <debug/debug.h>
#endif

//#define DEBUG_MEMORY_HOTPLUG 1

#define MAX_MEMORY_SEGMENTS 8

struct MemorySegment {
	struct page *pages = nullptr;
	vm_pindex_t base_pfn = -1;
	vm_pindex_t end_pfn = -1;
	vm_paddr_t base_phys = 0;
	vm_paddr_t end_phys = 0;
	size_t orig_size = 0;
	area_id pages_area_id = -1;
#if defined(CONFIG_MEMORY_HOTPLUG)
	void *dynamic_tag = nullptr;
	uint32_t nonsecure_tag = 0;
	uint8_t zone_index = 0;
	bool disconnect_pending = false;
#endif
};

// The page reserve an allocation of the certain priority must not touch.
static const size_t kPageReserveForPriority[] = {
	VM_PAGE_RESERVE_USER,  // user
	VM_PAGE_RESERVE_SYSTEM,// system
	0,                     // VIP
	0,                     // External
};

// The memory reserve an allocation of the certain priority must not touch.
static const size_t kMemoryReserveForPriority[] = {
	VM_MEMORY_RESERVE_USER,  // user
	VM_MEMORY_RESERVE_SYSTEM,// system
	0,                       // VIP
	VM_MEMORY_RESERVE_USER,  // External
};

#define SCRUB_SIZE 32
// this many pages will be cleared at once in the page scrubber thread

#define MAX_PAGE_WRITER_IO_PRIORITY K_URGENT_DISPLAY_PRIORITY
// maximum I/O priority of the page writer
#define MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD 10000
// the maximum I/O priority shall be reached when this many pages need to
// be written


#if defined(CONFIG_MEMORY_HOTPLUG)
#define VM_ZONE_COUNT 2
#else
#define VM_ZONE_COUNT 1
#endif

static unsigned int sIgnoredPages;

// Wait interval between page daemon runs.
static const time_t kIdleScanWaitInterval = SECONDS(1);    // 1 sec
static const time_t kBusyScanWaitInterval = MILLISECS(500);// 0.5 sec

// Number of idle runs after which we want to have processed the full active
// queue.
static const uint32_t kIdleRunsForFullQueue = 20;

// Maximum limit for the vm_page::usage_count.
static const int32_t kPageUsageMax = 64;
// vm_page::usage_count buff an accessed page receives in a scan.
static const int32_t kPageUsageAdvance = 3;
// vm_page::usage_count debuff an unaccessed page receives in a scan.
static const int32_t kPageUsageDecline = 1;

int gMappedPagesCount;

struct PageReservationWaiter
    : public DoublyLinkedListLinkImpl<PageReservationWaiter> {
	uint32_t dontTouch;// reserve not to touch
	uint32_t missing;  // pages missing for the reservation
	int32_t threadPriority;
	bool queued;
	cond_var_t cond{ "page reservation" };
	tid_t id = get_current_thread()->id;

	bool operator<(const PageReservationWaiter &other) const {
		// Implies an order by descending VM priority (ascending dontTouch)
		// and (secondarily) descending thread priority.
		if (dontTouch != other.dontTouch)
			return dontTouch < other.dontTouch;
		return threadPriority > other.threadPriority;
	}
};

typedef DoublyLinkedList<PageReservationWaiter> PageReservationWaiterList;

struct MemoryZone {
	VMPageQueue fFreePageQueue{ "free page queue" };
	VMPageQueue fClearPageQueue{ "clear page queue" };
	VMPageQueue fCachedPageQueue{ "cached page queue" };

	int fUnreservedFreePages = 0;
	int fUnsatisfiedPageReservations = 0;
	mutex_t fPageDeficitLock{ "page deficit lock" };

#if defined(CONFIG_DEBUG)
	uint32_t fMinimumAvailablePages = (uint32_t) -1;
#endif

	// This lock must be used whenever the free or clear page queues are changed.
	// If you need to work on both queues at the same time, you need to hold a write
	// lock, otherwise, a read lock suffices (each queue still has a spinlock to
	// guard against concurrent changes).
	rw_lock_t fFreePageQueuesLock{ "free page queues" };
	int fTotalPages = 0;

	// Minimum number of free pages the page daemon will try to achieve.
	uint32_t fFreePagesTarget = 0;
	uint32_t fFreeOrCachedPagesTarget = 0;
	uint32_t fInactivePagesTarget = 0;

	PageReservationWaiterList fPageReservationWaiters;

	/* For more complex stats that don't need 100% accuracy. Called
	 * periodically by a single thread (page daemon).
	 */
	void update_cumulative_stats() {
#if defined(CONFIG_DEBUG)
		uint32_t avail_pages = __atomic_load_n(&fUnreservedFreePages, __ATOMIC_RELAXED) + fCachedPageQueue.Count();
		uint32_t min_avail = __atomic_load_n(&fMinimumAvailablePages, __ATOMIC_RELAXED);
		min_avail = std::min(min_avail, avail_pages);
		__atomic_store_n(&fMinimumAvailablePages, min_avail, __ATOMIC_RELAXED);
#endif
	}

	void get_page_stats(struct page_stats *_pageStats) {
		_pageStats->totalPages = fTotalPages;
		_pageStats->totalFreePages = __atomic_load_n(&fUnreservedFreePages, __ATOMIC_RELAXED);
#if defined(CONFIG_DEBUG)
		_pageStats->lowestFreePages = __atomic_load_n(&fMinimumAvailablePages, __ATOMIC_RELAXED);
#endif
		_pageStats->unsatisfiedReservations = __atomic_load_n(&fUnsatisfiedPageReservations, __ATOMIC_RELAXED);
		_pageStats->cachedPages = fCachedPageQueue.Count();
	}
};

static VMPageQueue sModifiedPageQueue{ "modified page queue" };
static VMPageQueue sInactivePageQueue{ "inactive page queue" };
static VMPageQueue sActivePageQueue{ "active page queue" };

static MemoryZone sMemoryZone[VM_ZONE_COUNT];

/*
 * Protect memory hotplug regions. Any of the read locks
 * may be taken when iterating tbe memory segment array for
 * reading. Memory hotplug code will acquire the write lock
 * for both spinlock and mutex to remove segment from list.
 */

#if defined(CONFIG_MEMORY_HOTPLUG)
static mutex_t sMemoryHotplugMutex = MUTEX_INITIALIZER("memory hotplug mutex");
static DEFINE_RWLOCK(sPageSegmentsRWLock, "page segments");
static smp_rwspinlock sPageSegmentsRWSpinLock{ "page segments lock" };
#endif

static MemorySegment *sPageSegments[MAX_MEMORY_SEGMENTS];
static int sPageSegmentCount;

static int sModifiedTemporaryPages;

static VMPhysicalPageMapper *sPhysicalPageMapper;

static DEFINE_MUTEX_ETC(sAvailableMemoryLock, "available memory");
static cond_var_t sFreePageCondition{ "free page condition" };

off_t sAvailableMemory = 0;
off_t sNeededMemory = 0;
off_t sTotalMemory = 0;

bool vm_page_allocator_active = false;

#if defined(CONFIG_MEMORY_HOTPLUG)

#if defined(CONFIG_QEMU)
static struct dynamic_memory_driver sQEmuDriver = {
	[](vm_paddr_t phys, size_t size, void **result_tag) -> int {
	    return 0;
	},
	[](vm_paddr_t phys, size_t size, void *tag) {

	},
	[]() -> size_t {
	    return 0x200000;
	}
};

static struct dynamic_memory_driver *sDynamicMemoryDriver = &sQEmuDriver;
#else
static struct dynamic_memory_driver *sDynamicMemoryDriver;
#endif
#endif

struct DaemonCondition {
	constexpr explicit DaemonCondition(const char *name = "daemon condition") : fCondition(name) {
	}

	bool Lock() {
		return mutex_lock(&fLock) == 0;
	}

	void Unlock() {
		mutex_unlock(&fLock);
	}

	bool Wait(time_t timeout, bool clearActivated) {
		MutexLocker locker(fLock);

		if (clearActivated)
			fActivated = false;
		else if (fActivated)
			return true;

		DEFINE_CONDVAR_ENTRY(entry);
		fCondition.Add(&entry);

		locker.Unlock();

		return entry.Wait(B_RELATIVE_TIMEOUT, timeout) == 0;
	}

	void WakeUp() {
		if (fActivated)
			return;

		MutexLocker locker(fLock);
		fActivated = true;
		fCondition.NotifyOne();
	}

	void ClearActivated() {
		MutexLocker locker(fLock);
		fActivated = false;
	}

private:
	mutex_t fLock{ "daemon condition" };
	cond_var_t fCondition;
	bool fActivated{ false };
};

static DaemonCondition sPageWriterCondition{ "page_writer" };
static DaemonCondition sPageDaemonCondition[VM_ZONE_COUNT];

inline void page::InitState(uint8_t newState) {
	state = newState;
}

inline void page::SetState(uint8_t newState) {
	state = newState;
}

inline void page::Init(vm_pindex_t pageNumber) {
	pfn = pageNumber;
	InitState(PAGE_STATE_FREE);
	new (&mappings) vm_page_mappings();
	wired_count = 0;
	usage_count = 0;
	tag = 0xffff;
	busy_writing = false;
	SetCacheRef(nullptr);
#ifdef CONFIG_DEBUG
	queue = nullptr;
#endif
}

static bool
do_active_paging(const page_stats &pageStats, MemoryZone &zone) {
	return pageStats.totalFreePages + pageStats.cachedPages < pageStats.unsatisfiedReservations + (int32_t) zone.fFreeOrCachedPagesTarget;
}

static uint32_t
reserve_some_pages_from_zone(uint32_t count, uint32_t dontTouch,
                             MemoryZone &zone) {
	int32_t freePages = __atomic_load_n(
	        &zone.fUnreservedFreePages, __ATOMIC_ACQUIRE);

	while (true) {
		if (freePages <= (int32_t) dontTouch)
			return 0;

		int32_t toReserve = std::min(count, freePages - dontTouch);

		if (__atomic_compare_exchange_n(
		            &zone.fUnreservedFreePages, &freePages,
		            freePages - toReserve, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
			return toReserve;
		}

		// the count changed in the meantime -- retry
	}
}

static void
wake_up_page_reservation_waiters_for_zone(MemoryZone &zone) {
	MutexLocker pageDeficitLocker(zone.fPageDeficitLock);

	// TODO: If this is a low priority thread, we might want to disable
	// interrupts or otherwise ensure that we aren't unscheduled. Otherwise
	// high priority threads wait be kept waiting while a medium priority thread
	// prevents us from running.

	while (PageReservationWaiter *waiter =
	               zone.fPageReservationWaiters.Head()) {
		int32_t reserved = reserve_some_pages_from_zone(waiter->missing,
		                                                waiter->dontTouch, zone);

		if (reserved == 0)
			return;

		__atomic_fetch_sub(
		        &zone.fUnsatisfiedPageReservations, reserved,
		        __ATOMIC_RELEASE);

		waiter->missing -= reserved;

		if (waiter->missing > 0)
			return;

		ASSERT(waiter->queued);
		zone.fPageReservationWaiters.Remove(waiter);
		waiter->queued = false;

		waiter->cond.NotifyAll();
	}
}

static inline void
unreserve_pages_for_zone(uint32_t count, MemoryZone &zone) {
	__atomic_fetch_add(&zone.fUnreservedFreePages, count, __ATOMIC_RELEASE);

	if (__atomic_load_n(&zone.fUnsatisfiedPageReservations, __ATOMIC_RELAXED) != 0)
		wake_up_page_reservation_waiters_for_zone(zone);
}


static void free_page(struct page *page, bool clear) {
	if (page->IsMapped()) {
		panic("Trying to free page %p which is still mapped", page);
	}

	VMPageQueue *fromQueue;

	MemoryZone *pageZone = &sMemoryZone[page->Zone()];

	switch (page->State()) {
		case PAGE_STATE_ACTIVE:
			fromQueue = &sActivePageQueue;
			break;
		case PAGE_STATE_INACTIVE:
			fromQueue = &sInactivePageQueue;
			break;
		case PAGE_STATE_MODIFIED:
			fromQueue = &sModifiedPageQueue;
			break;
		case PAGE_STATE_CACHED:
			fromQueue = &pageZone->fCachedPageQueue;
			break;
		case PAGE_STATE_FREE:
		case PAGE_STATE_CLEAR:
			panic("free_page(): page %p already free", page);
		case PAGE_STATE_WIRED:
		case PAGE_STATE_UNUSED:
			fromQueue = nullptr;
			break;
		default:
			panic("free_page(): page %p in invalid state %d",
			      page, page->State());
			return;
	}

	if (page->CacheRef() != nullptr)
		panic("to be freed page %p has cache", page);
	if (page->IsMapped())
		panic("to be freed page %p has mappings", page);

	if (fromQueue != nullptr)
		fromQueue->RemoveUnlocked(page);

	ReadLocker locker(pageZone->fFreePageQueuesLock);

	if (clear) {
		page->SetState(PAGE_STATE_CLEAR);
		pageZone->fClearPageQueue.PrependUnlocked(page);
	} else {
		page->SetState(PAGE_STATE_FREE);
		pageZone->fFreePageQueue.PrependUnlocked(page);
		sFreePageCondition.NotifyAll();
	}
}


/*!	The caller must make sure that no-one else tries to change the page's state
	while the function is called. If the page has a cache, this can be done by
	locking the cache.
*/
static void set_page_state(struct page *page, int pageState) {
	if (pageState == page->State())
		return;

	MemoryZone *pageZone = &sMemoryZone[page->Zone()];

	VMPageQueue *fromQueue;

	switch (page->State()) {
		case PAGE_STATE_ACTIVE:
			fromQueue = &sActivePageQueue;
			break;
		case PAGE_STATE_INACTIVE:
			fromQueue = &sInactivePageQueue;
			break;
		case PAGE_STATE_MODIFIED:
			fromQueue = &sModifiedPageQueue;
			break;
		case PAGE_STATE_CACHED:
			fromQueue = &pageZone->fCachedPageQueue;
			break;
		case PAGE_STATE_FREE:
		case PAGE_STATE_CLEAR:
			panic("set_page_state(): page %p is free/clear", page);
		case PAGE_STATE_WIRED:
		case PAGE_STATE_UNUSED:
			fromQueue = nullptr;
			break;
		default:
			panic("set_page_state(): page %p in invalid state %d",
			      page, page->State());
			return;
	}

	VMPageQueue *toQueue;

	switch (pageState) {
		case PAGE_STATE_ACTIVE:
			toQueue = &sActivePageQueue;
			break;
		case PAGE_STATE_INACTIVE:
			toQueue = &sInactivePageQueue;
			break;
		case PAGE_STATE_MODIFIED:
			toQueue = &sModifiedPageQueue;
			break;
		case PAGE_STATE_CACHED:
			ASSERT(!page->IsMapped());
			ASSERT(!page->modified);
			toQueue = &pageZone->fCachedPageQueue;
			break;
		case PAGE_STATE_FREE:
		case PAGE_STATE_CLEAR:
			panic("set_page_state(): target state is free/clear");
		case PAGE_STATE_WIRED:
		case PAGE_STATE_UNUSED:
			toQueue = nullptr;
			break;
		default:
			panic("set_page_state(): invalid target state %d", pageState);
	}

	VMCache *cache = page->Cache();

	if (cache != nullptr && cache->temporary) {
		if (pageState == PAGE_STATE_MODIFIED)
			__atomic_fetch_add(&sModifiedTemporaryPages, 1, __ATOMIC_RELAXED);
		else if (page->State() == PAGE_STATE_MODIFIED)
			__atomic_fetch_sub(&sModifiedTemporaryPages, 1, __ATOMIC_RELAXED);
	}

	// move the page
	if (toQueue == fromQueue) {
		// Note: Theoretically we are required to lock when changing the page
		// state, even if we don't change the queue. We actually don't have to
		// do this, though, since only for the active queue there are different
		// page states and active pages have a cache that must be locked at
		// this point. So we rely on the fact that everyone must lock the cache
		// before trying to change/interpret the page state.
		ASSERT(cache != nullptr);
		cache->AssertLocked();
		page->SetState(pageState);
	} else {
		if (fromQueue != nullptr)
			fromQueue->RemoveUnlocked(page);

		page->SetState(pageState);

		if (toQueue != nullptr)
			toQueue->AppendUnlocked(page);
	}
}

/*! Moves a previously modified page into a now appropriate queue.
	The page queues must not be locked.
*/
static void move_page_to_appropriate_queue(struct page *page) {
	// Note, this logic must be in sync with what the page daemon does.
	int32_t state;
	if (page->IsMapped())
		state = PAGE_STATE_ACTIVE;
	else if (page->modified)
		state = PAGE_STATE_MODIFIED;
	else
		state = PAGE_STATE_CACHED;

	// TODO: If free + cached pages are low, we might directly want to free the
	// page.
	set_page_state(page, state);
}

static void clear_page(struct page *page) {
	vm_memset_physical(page_to_phys(page), 0, PAGE_SIZE);
}

static void mark_page_range_in_use(vm_pindex_t startPage, vm_pindex_t length, bool wired) {
	for (int index = 0; index < sPageSegmentCount; ++index) {
		if (!sPageSegments[index])
			continue;

		vm_pindex_t firstIndex = MAX(startPage, sPageSegments[index]->base_pfn);
		vm_pindex_t endIndex = MIN(startPage + length, sPageSegments[index]->end_pfn);

#if defined(CONFIG_MEMORY_HOTPLUG)
		MemoryZone *zone = &sMemoryZone[sPageSegments[index]->zone_index];
#else
		MemoryZone *zone = &sMemoryZone[0];
#endif

		WriteLocker locker(zone->fFreePageQueuesLock);

		for (vm_pindex_t i = firstIndex; i < endIndex; i++) {
			struct page *page = &sPageSegments[index]->pages[i - sPageSegments[index]->base_pfn];
			switch (page->State()) {
				case PAGE_STATE_FREE:
				case PAGE_STATE_CLEAR: {
					// TODO: This violates the page reservation policy, since we remove pages from
					// the free/clear queues without having reserved them before. This should happen
					// in the early boot process only, though.
					VMPageQueue &queue =
					        page->State() == PAGE_STATE_FREE ? zone->fFreePageQueue : zone->fClearPageQueue;

					queue.Remove(page);
					page->SetState(wired ? PAGE_STATE_WIRED : PAGE_STATE_UNUSED);
					page->busy = false;
					__atomic_fetch_sub(&zone->fUnreservedFreePages, 1, __ATOMIC_RELAXED);
					break;
				}
				case PAGE_STATE_WIRED:
				case PAGE_STATE_UNUSED:
					break;
				case PAGE_STATE_ACTIVE:
				case PAGE_STATE_INACTIVE:
				case PAGE_STATE_MODIFIED:
				case PAGE_STATE_CACHED:
				default:
					// uh
					printk(KERN_ERR, "mark_page_range_in_use: page %#" B_PRIxPHYSADDR " in non-free state %d!\n", (vm_paddr_t) i, page->State());
					break;
			}
		}
	}
}


/*!
	This is a background thread that wakes up every now and then (every 100ms)
	and moves some pages from the free queue over to the clear queue.
	Given enough time, it will clear out all pages from the free queue - we
	could probably slow it down after having reached a certain threshold.
*/
static int page_scrubber(void *) {
	cond_var_entry entry;

	// Don't trigger at system boot to avoid extra trigger of kernel
	// workers when REE is busy
#if !defined(CONFIG_NO_TRUSTZONE)
	snooze(SECONDS(60));
#endif

	for (;;) {
		bool nothingToDo = true;

#if !defined(CONFIG_NO_TRUSTZONE)
		// Wait at least 500ms between runs
		snooze(MILLISECS(500));
#else
		snooze(MILLISECS(100));
#endif

		for (auto &zoneIndex : sMemoryZone) {
			MemoryZone *zone = &zoneIndex;

			if (zone->fFreePageQueue.Count() == 0 ||
			    __atomic_load_n(&zone->fUnreservedFreePages,
			                    __ATOMIC_RELAXED) < (int32_t) zone->fFreePagesTarget) {
				continue;
			}

			nothingToDo = false;

#if !defined(CONFIG_NO_TRUSTZONE)
			if(is_ns_irq_pending()) {
				preempt();
			}
#endif

			// Since we temporarily remove pages from the free pages reserve,
			// we must make sure we don't cause a violation of the page
			// reservation warranty. The following is usually stricter than
			// necessary, because we don't have information on how many of the
			// reserved pages have already been allocated.
			int32_t reserved = reserve_some_pages_from_zone(SCRUB_SIZE,
			                                                kPageReserveForPriority[GFP_PRIO_USER], *zone);

			if (reserved == 0)
				continue;

			// get some pages from the free queue
			ReadLocker locker(zone->fFreePageQueuesLock);

			struct page *page[SCRUB_SIZE];
			int32_t scrubCount = 0;
			for (int32_t i = 0; i < reserved; i++) {
				page[i] = zone->fFreePageQueue.RemoveHeadUnlocked();
				if (page[i] == nullptr)
					break;

				page[i]->SetState(PAGE_STATE_ACTIVE);
				page[i]->busy = true;
				scrubCount++;
			}

			locker.Unlock();

			if (scrubCount == 0) {
				unreserve_pages_for_zone(reserved, *zone);
				continue;
			}

			// clear them
			for (int32_t i = 0; i < scrubCount; i++)
				clear_page(page[i]);

			locker.Lock();

			// and put them into the clear queue
			for (int32_t i = 0; i < scrubCount; i++) {
				page[i]->SetState(PAGE_STATE_CLEAR);
				page[i]->busy = false;
				zone->fClearPageQueue.PrependUnlocked(page[i]);
			}

			locker.Unlock();

			unreserve_pages_for_zone(reserved, *zone);
		}

		sFreePageCondition.Add(&entry);

		if (nothingToDo) {
			entry.Wait();
		} else {
			entry.Wait(B_RELATIVE_TIMEOUT, SECONDS(1));
		}
	}

	return 0;
}

static void init_page_marker(struct page &marker, [[maybe_unused]] MemoryZone &zone) {
	marker.SetCacheRef(nullptr);
	marker.InitState(PAGE_STATE_UNUSED);
	marker.busy = true;
#ifdef CONFIG_DEBUG
	marker.queue = nullptr;
#endif
#if defined(CONFIG_MEMORY_HOTPLUG)
	marker.zone_index = 0;
#endif
}

static void remove_page_marker(struct page &marker, MemoryZone &zone) {
	VMPageQueue *fromQueue = nullptr;

	switch (marker.State()) {
		case PAGE_STATE_ACTIVE:
			fromQueue = &sActivePageQueue;
			break;
		case PAGE_STATE_INACTIVE:
			fromQueue = &sInactivePageQueue;
			break;
		case PAGE_STATE_MODIFIED:
			fromQueue = &sModifiedPageQueue;
			break;
		case PAGE_STATE_CACHED:
			fromQueue = &zone.fCachedPageQueue;
			break;
		case PAGE_STATE_FREE:
			fromQueue = &zone.fFreePageQueue;
			break;
		case PAGE_STATE_CLEAR:
			fromQueue = &zone.fClearPageQueue;
			break;
		default:
			break;
	}

	if (fromQueue) {
		fromQueue->RemoveUnlocked(&marker);
	}

	marker.SetState(PAGE_STATE_UNUSED);
}

static page *
next_modified_page(vm_pindex_t &maxPagesToSee) {
	InterruptsSpinLocker locker(sModifiedPageQueue.GetLock());

	while (maxPagesToSee > 0) {
		struct page *page = sModifiedPageQueue.Head();
		if (page == nullptr)
			return nullptr;

		sModifiedPageQueue.Requeue(page, true);

		maxPagesToSee--;

		if (!page->busy)
			return page;
	}

	return nullptr;
}

class PageWriteTransfer;
class PageWriteWrapper;

class PageWriterRun {
public:
	int Init(uint32_t maxPages);

	void PrepareNextRun();
	void AddPage(struct page *page);
	uint32_t Go();

	void PageWritten(PageWriteTransfer *transfer, int status,
	                 bool partialTransfer, size_t bytesTransferred);

private:
	uint32_t fMaxPages{ 0 };
	uint32_t fWrapperCount{ 0 };
	uint32_t fTransferCount{ 0 };
	int fPendingTransfers{ 0 };
	PageWriteWrapper *fWrappers{};
	PageWriteTransfer *fTransfers{};
	cond_var_t fAllFinishedCondition{ "page writer wait for I/O" };
};

class PageWriteTransfer final : public AsyncIOCallback {
public:
	void SetTo(PageWriterRun *run, struct page *page, int32_t maxPages);
	bool AddPage(struct page *page);

	int Schedule(uint32_t flags);

	void SetStatus(int status, size_t transferred);

	[[nodiscard]] int Status() const { return fStatus; }
	[[nodiscard]] struct VMCache *Cache() const { return fCache; }
	[[nodiscard]] uint32_t PageCount() const { return fPageCount; }

	void IOFinished(int status, bool partialTransfer,
	                size_t bytesTransferred) override;

private:
	PageWriterRun *fRun{};
	struct VMCache *fCache{};
	off_t fOffset{};
	off_t fPageCount{};
	int32_t fMaxPages{};
	int fStatus{};
	uint32_t fVecCount{};
	generic_iovec fVecs[32]{};// TODO: make dynamic/configurable
};

class PageWriteWrapper : boost::noncopyable {
public:
	~PageWriteWrapper() {
		if (fIsActive)
			panic("page write wrapper going out of scope but isn't completed");
	}

	void SetTo(struct page *page);
	bool Done(int result);

private:
	struct page *fPage{ nullptr };
	struct VMCache *fCache{ nullptr };
	bool fIsActive{ false };
};


/*!	The page's cache must be locked.
*/
void PageWriteWrapper::SetTo(struct page *page) {
	if (page->busy)
		panic("setting page write wrapper to busy page");

	if (fIsActive)
		panic("re-setting page write wrapper that isn't completed");

	fPage = page;
	fCache = page->Cache();
	fIsActive = true;

	fPage->busy = true;
	fPage->busy_writing = true;

	// We have a modified page -- however, while we're writing it back,
	// the page might still be mapped. In order not to lose any changes to the
	// page, we mark it clean before actually writing it back; if
	// writing the page fails for some reason, we'll just keep it in the
	// modified page list, but that should happen only rarely.

	// If the page is changed after we cleared the dirty flag, but before we
	// had the chance to write it back, then we'll write it again later -- that
	// will probably not happen that often, though.

	vm_clear_map_flags(fPage, PAGE_MODIFIED);
}


/*!	The page's cache must be locked.
	The page queues must not be locked.
	\return \c true if the page was written successfully respectively could be
		handled somehow, \c false otherwise.
*/
bool PageWriteWrapper::Done(int result) {
	if (!fIsActive)
		panic("completing page write wrapper that is not active");

	fPage->busy = false;
	// Set unbusy and notify later by hand, since we might free the page.

	bool success = true;

	if (result == 0) {
		// put it into the active/inactive queue
		move_page_to_appropriate_queue(fPage);
		fPage->busy_writing = false;
	} else {
		// Writing the page failed. One reason would be that the cache has been
		// shrunk and the page does no longer belong to the file. Otherwise the
		// actual I/O failed, in which case we'll simply keep the page modified.

		if (!fPage->busy_writing) {
			// The busy_writing flag was cleared. That means the cache has been
			// shrunk while we were trying to write the page and we have to free
			// it now.
			vm_remove_all_page_mappings(fPage);
			// TODO: Unmapping should already happen when resizing the cache!
			uint8_t zone = fPage->Zone();
			fCache->RemovePage(fPage);
			free_page(fPage, false);
			unreserve_pages_for_zone(1, sMemoryZone[zone]);
		} else {
			// Writing the page failed -- mark the page modified and move it to
			// an appropriate queue other than the modified queue, so we don't
			// keep trying to write it over and over again. We keep
			// non-temporary pages in the modified queue, though, so they don't
			// get lost in the inactive queue.
			printk(KERN_ERR, "PageWriteWrapper: Failed to write page %p: %d\n", fPage,
			       result);

			fPage->modified = true;
			if (!fCache->temporary)
				set_page_state(fPage, PAGE_STATE_MODIFIED);
			else if (fPage->IsMapped())
				set_page_state(fPage, PAGE_STATE_ACTIVE);
			else
				set_page_state(fPage, PAGE_STATE_INACTIVE);

			fPage->busy_writing = false;

			success = false;
		}
	}

	fCache->NotifyPageEvents(fPage, PAGE_EVENT_NOT_BUSY);
	fIsActive = false;

	return success;
}

/*!	The page's cache must be locked.
*/
void PageWriteTransfer::SetTo(PageWriterRun *run, struct page *page, int32_t maxPages) {
	fRun = run;
	fCache = page->Cache();
	fOffset = page->cache_offset;
	fPageCount = 1;
	fMaxPages = maxPages;
	fStatus = 0;

	fVecs[0].base = page_to_phys(page);
	fVecs[0].length = PAGE_SIZE;
	fVecCount = 1;
}


/*!	The page's cache must be locked.
*/
bool PageWriteTransfer::AddPage(struct page *page) {
	if (page->Cache() != fCache || (fMaxPages >= 0 && fPageCount >= fMaxPages))
		return false;

	vm_paddr_t nextBase = fVecs[fVecCount - 1].base + fVecs[fVecCount - 1].length;

	if (page_to_phys(page) == nextBase && (off_t) page->cache_offset == fOffset + fPageCount) {
		// append to last iovec
		fVecs[fVecCount - 1].length += PAGE_SIZE;
		fPageCount++;
		return true;
	}

	nextBase = fVecs[0].base - PAGE_SIZE;
	if (page_to_phys(page) == nextBase && (off_t) page->cache_offset == fOffset - 1) {
		// prepend to first iovec and adjust offset
		fVecs[0].base = nextBase;
		fVecs[0].length += PAGE_SIZE;
		fOffset = page->cache_offset;
		fPageCount++;
		return true;
	}

	if (((off_t) page->cache_offset == fOffset + fPageCount || (off_t) page->cache_offset == fOffset - 1) && fVecCount < sizeof(fVecs) / sizeof(fVecs[0])) {
		// not physically contiguous or not in the right order
		uint32_t vectorIndex;
		if ((off_t) page->cache_offset < fOffset) {
			// we are pre-pending another vector, move the other vecs
			for (uint32_t i = fVecCount; i > 0; i--)
				fVecs[i] = fVecs[i - 1];

			fOffset = page->cache_offset;
			vectorIndex = 0;
		} else
			vectorIndex = fVecCount;

		fVecs[vectorIndex].base = page_to_phys(page);
		fVecs[vectorIndex].length = PAGE_SIZE;

		fVecCount++;
		fPageCount++;
		return true;
	}

	return false;
}

int PageWriteTransfer::Schedule(uint32_t flags) {
	off_t writeOffset = (off_t) fOffset << PAGE_SHIFT;
	size_t writeLength = (size_t) fPageCount << PAGE_SHIFT;

	if (fRun != nullptr) {
		return fCache->WriteAsync(writeOffset, fVecs, fVecCount, writeLength,
		                          flags | GENERIC_IO_USE_PHYSICAL, this);
	}

	int status = fCache->Write(writeOffset, fVecs,
	                           flags | GENERIC_IO_USE_PHYSICAL, fVecCount, &writeLength);

	SetStatus(status, writeLength);
	return fStatus;
}

void PageWriteTransfer::SetStatus(int status, size_t transferred) {
	// only succeed if all pages up to the last one have been written fully
	// and the last page has at least been written partially
	if (status == 0 && transferred <= size_t((fPageCount - 1) * PAGE_SIZE))
		status = -EIO;

	fStatus = status;
}

void PageWriteTransfer::IOFinished(int status, bool partialTransfer,
                                   size_t bytesTransferred) {
	SetStatus(status, bytesTransferred);
	fRun->PageWritten(this, fStatus, partialTransfer, bytesTransferred);
}

int PageWriterRun::Init(uint32_t maxPages) {
	fMaxPages = maxPages;
	fWrapperCount = 0;
	fTransferCount = 0;
	fPendingTransfers = 0;

	fWrappers = new (std::nothrow) PageWriteWrapper[maxPages];
	fTransfers = new (std::nothrow) PageWriteTransfer[maxPages];
	if (fWrappers == nullptr || fTransfers == nullptr)
		return -ENOMEM;

	return 0;
}

void PageWriterRun::PrepareNextRun() {
	fWrapperCount = 0;
	fTransferCount = 0;
	__atomic_store_n(&fPendingTransfers, 0, __ATOMIC_RELEASE);
}

/*!	The page's cache must be locked.
*/
void PageWriterRun::AddPage(struct page *page) {
	fWrappers[fWrapperCount++].SetTo(page);

	if (fTransferCount == 0 || !fTransfers[fTransferCount - 1].AddPage(page)) {
		fTransfers[fTransferCount++].SetTo(this, page,
		                                   page->Cache()->MaxPagesPerAsyncWrite());
	}
}

/*!	Writes all pages previously added.
	\return The number of pages that could not be written or otherwise handled.
*/
uint32_t PageWriterRun::Go() {
	__atomic_store_n(&fPendingTransfers, fTransferCount, __ATOMIC_RELEASE);

	cond_var_entry waitEntry;
	fAllFinishedCondition.Add(&waitEntry);

	// schedule writes
	for (uint32_t i = 0; i < fTransferCount; i++)
		fTransfers[i].Schedule(GENERIC_IO_VIP);

	waitEntry.Wait();

	// mark pages depending on whether they could be written or not

	uint32_t failedPages = 0;
	uint32_t wrapperIndex = 0;
	for (uint32_t i = 0; i < fTransferCount; i++) {
		PageWriteTransfer &transfer = fTransfers[i];
		transfer.Cache()->Lock();

		for (uint32_t j = 0; j < transfer.PageCount(); j++) {
			if (!fWrappers[wrapperIndex++].Done(transfer.Status()))
				failedPages++;
		}

		transfer.Cache()->Unlock();
	}

	ASSERT(wrapperIndex == fWrapperCount);

	for (uint32_t i = 0; i < fTransferCount; i++) {
		PageWriteTransfer &transfer = fTransfers[i];
		struct VMCache *cache = transfer.Cache();

		// We've acquired a references for each page
		for (uint32_t j = 0; j < transfer.PageCount(); j++) {
			// We release the cache references after all pages were made
			// unbusy again - otherwise releasing a vnode could deadlock.
			cache->ReleaseStoreRef();
			cache->ReleaseRef();
		}
	}

	return failedPages;
}

void PageWriterRun::PageWritten([[maybe_unused]] PageWriteTransfer *transfer, [[maybe_unused]] int status,
                                [[maybe_unused]] bool partialTransfer, [[maybe_unused]] size_t bytesTransferred) {
	if (__atomic_fetch_sub(&fPendingTransfers, 1, __ATOMIC_RELAXED) == 1)
		fAllFinishedCondition.NotifyAll();
}

class PageCacheLocker {
public:
	inline PageCacheLocker(page *page,
	                       bool dontWait = true);
	inline ~PageCacheLocker();

	bool IsLocked() { return fPage != nullptr; }

	bool Lock(page *page, bool dontWait = true);
	void Unlock();

private:
	bool _IgnorePage(page *page);

	page *fPage;
};


bool PageCacheLocker::_IgnorePage(page *page) {
	if (page->busy || page->state == PAGE_STATE_WIRED || page->state == PAGE_STATE_FREE || page->state == PAGE_STATE_UNUSED || page->wired_count > 0)
		return true;

	return false;
}


bool PageCacheLocker::Lock(page *page, bool dontWait) {
	if (_IgnorePage(page))
		return false;

	// Grab a reference to this cache.
	VMCache *cache = vm_cache_acquire_locked_page_cache(page, dontWait);
	if (cache == nullptr)
		return false;

	if (_IgnorePage(page)) {
		cache->ReleaseRefAndUnlock();
		return false;
	}

	fPage = page;
	return true;
}


void PageCacheLocker::Unlock() {
	if (fPage == nullptr)
		return;

	fPage->Cache()->ReleaseRefAndUnlock();

	fPage = nullptr;
}

PageCacheLocker::PageCacheLocker(page *page, bool dontWait)
    : fPage(nullptr) {
	Lock(page, dontWait);
}


PageCacheLocker::~PageCacheLocker() {
	Unlock();
}

/*!	The page writer continuously takes some pages from the modified
	queue, writes them back, and moves them back to the active queue.
	It runs in its own thread, and is only there to keep the number
	of modified pages low, so that more pages can be reused with
	fewer costs.
*/
int page_writer(void * /*unused*/) {
	const uint32_t kNumPages = vm_page_num_pages() > 16384 ? 256 : 32;

	PageWriterRun run;
	if (run.Init(kNumPages) != 0) {
		panic("page writer: Failed to init PageWriterRun!");
	}

	vm_pindex_t pagesSinceLastSuccessfulWrite = 0;

	while (true) {
		// TODO: Maybe wait shorter when memory is low!
		if (sModifiedPageQueue.Count() < kNumPages) {
			sPageWriterCondition.Wait(SECONDS(3), true);
			// all 3 seconds when no one triggers us
		}

		vm_pindex_t modifiedPages = sModifiedPageQueue.Count();

		if (modifiedPages == 0)
			continue;

		if (modifiedPages <= pagesSinceLastSuccessfulWrite) {
			// We ran through the whole queue without being able to write a
			// single page. Take a break.
			snooze(MILLISECS(500));
			pagesSinceLastSuccessfulWrite = 0;
		}

		page_stats pageStats;
		sMemoryZone[0].get_page_stats(&pageStats);

		bool activePaging = do_active_paging(pageStats, sMemoryZone[0]);

		// depending on how urgent it becomes to get pages to disk, we adjust
		// our I/O priority
		uint32_t lowPagesState = low_resource_state(B_KERNEL_RESOURCE_PAGES);
		int32_t ioPriority = K_IDLE_PRIORITY;
		if (lowPagesState >= B_LOW_RESOURCE_CRITICAL || modifiedPages > MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD) {
			ioPriority = MAX_PAGE_WRITER_IO_PRIORITY;
		} else {
			ioPriority = (uint64_t) MAX_PAGE_WRITER_IO_PRIORITY * modifiedPages / MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD;
		}

		thread_set_io_priority(ioPriority);

		uint32_t numPages = 0;
		run.PrepareNextRun();

		// TODO: make this laptop friendly, too (ie. only start doing
		// something if someone else did something or there is really
		// enough to do).

		// collect pages to be written
		vm_pindex_t maxPagesToSee = modifiedPages;

		while (numPages < kNumPages && maxPagesToSee > 0) {
			struct page *page = next_modified_page(maxPagesToSee);
			if (page == nullptr)
				break;

			PageCacheLocker cacheLocker(page, false);
			if (!cacheLocker.IsLocked())
				continue;

			VMCache *cache = page->Cache();

			// If the page is busy or its state has changed while we were
			// locking the cache, just ignore it.
			if (page->busy || page->State() != PAGE_STATE_MODIFIED)
				continue;

			// Don't write back wired (locked) pages.
			if (page->WiredCount() > 0) {
				set_page_state(page, PAGE_STATE_ACTIVE);
				continue;
			}

			// Write back temporary pages only when we're actively paging.
			if (cache->temporary && (!activePaging || !cache->CanWritePage(
			                                                  (off_t) page->cache_offset << PAGE_SHIFT))) {
				// We can't/don't want to do anything with this page, so move it
				// to one of the other queues.
				if (page->mappings.IsEmpty())
					set_page_state(page, PAGE_STATE_INACTIVE);
				else
					set_page_state(page, PAGE_STATE_ACTIVE);

				continue;
			}

			// We need our own reference to the store, as it might currently be
			// destroyed.
			if (cache->AcquireUnreferencedStoreRef() != 0) {
				cacheLocker.Unlock();
				preempt();
				continue;
			}

			run.AddPage(page);
			// TODO: We're possibly adding pages of different caches and
			// thus maybe of different underlying file systems here. This
			// is a potential problem for loop file systems/devices, since
			// we could mark a page busy that would need to be accessed
			// when writing back another page, thus causing a deadlock.

			cache->AcquireRefLocked();
			numPages++;
		}

		if (numPages == 0)
			continue;

		// write pages to disk and do all the cleanup
		uint32_t failedPages = run.Go();

		if (failedPages == numPages)
			pagesSinceLastSuccessfulWrite += modifiedPages - maxPagesToSee;
		else
			pagesSinceLastSuccessfulWrite = 0;
	}

	return 0;
}


static struct page *find_cached_page_candidate(struct page &marker, MemoryZone &zone) {
	InterruptsSpinLocker locker(zone.fCachedPageQueue.GetLock());
	struct page *page;

	if (marker.State() == PAGE_STATE_UNUSED) {
		// Get the first free pages of the (in)active queue
		page = zone.fCachedPageQueue.Head();
	} else {
		// Get the next page of the current queue
		if (marker.State() != PAGE_STATE_CACHED) {
			panic("invalid marker %p state", &marker);
		}

		page = zone.fCachedPageQueue.Next(&marker);
		zone.fCachedPageQueue.Remove(&marker);
		marker.SetState(PAGE_STATE_UNUSED);
	}

	while (page != nullptr) {
		if (!page->busy) {
			// we found a candidate, insert marker
			marker.SetState(PAGE_STATE_CACHED);
			zone.fCachedPageQueue.InsertAfter(page, &marker);
			return page;
		}

		page = zone.fCachedPageQueue.Next(page);
	}

	return nullptr;
}

static bool free_cached_page(struct page *page, bool dontWait, MemoryZone &zone, bool markBusy = false) {
	// try to lock the page's cache
	if (vm_cache_acquire_locked_page_cache(page, dontWait) == nullptr)
		return false;
	VMCache *cache = page->Cache();

	AutoLocker<VMCache> cacheLocker(cache, true);
	MethodDeleter<VMCache> _2(cache, &VMCache::ReleaseRefLocked);

	// check again if that page is still a candidate
	if (page->busy || page->State() != PAGE_STATE_CACHED)
		return false;

	ASSERT(!page->IsMapped());
	ASSERT(!page->modified);

	// we can now steal this page

	cache->RemovePage(page);
	// Now the page doesn't have cache anymore, so no one else (e.g.
	// vm_page_allocate_page_run() can pick it up), since they would be
	// required to lock the cache first, which would fail.

	zone.fCachedPageQueue.RemoveUnlocked(page);

	if (markBusy) {
		page->busy = 1;
	}

	return true;
}

static uint32_t free_cached_pages(uint32_t pagesToFree, bool dontWait, MemoryZone &zone) {
	struct page marker = {};
	init_page_marker(marker, zone);

	uint32_t pagesFreed = 0;

	while (pagesFreed < pagesToFree) {
		struct page *page = find_cached_page_candidate(marker, zone);
		if (page == nullptr)
			break;

		if (free_cached_page(page, dontWait, zone)) {
			ReadLocker locker(zone.fFreePageQueuesLock);
			page->SetState(PAGE_STATE_FREE);
			zone.fFreePageQueue.PrependUnlocked(page);
			locker.Unlock();

			pagesFreed++;
		}
	}

	remove_page_marker(marker, zone);

	sFreePageCondition.NotifyAll();

	return pagesFreed;
}


static void
idle_scan_active_pages([[maybe_unused]] page_stats &pageStats, MemoryZone &zone) {
	VMPageQueue &queue = sActivePageQueue;
	const bool virtualAccessedBit = VMAddressSpace::Kernel()->TranslationMap()->VirtualAccessedBit();

	struct page marker = {};
	init_page_marker(marker, zone);

	// We want to scan the whole queue in roughly kIdleRunsForFullQueue runs.
	uint32_t maxToScan = queue.Count() / kIdleRunsForFullQueue + 1;

	while (maxToScan > 0) {
		maxToScan--;

		InterruptsSpinLocker queueLocker(queue.GetLock());

		struct page *page;

		if (marker.State() == PAGE_STATE_UNUSED) {
			page = queue.Head();
		} else {
			page = queue.Next(&marker);
			queue.Remove(&marker);
			marker.SetState(PAGE_STATE_UNUSED);
		}

		if (page == nullptr)
			break;

		marker.SetState(PAGE_STATE_ACTIVE);
		queue.InsertAfter(page, &marker);

		if (page->busy)
			continue;

#if defined(CONFIG_MEMORY_HOTPLUG)
		if (page->segment->disconnect_pending)
			continue;
#endif

		// Ignore early
		if (virtualAccessedBit && page->WiredCount() > 0)
			continue;

		queueLocker.Unlock();

		// lock the page's cache
		VMCache *cache = vm_cache_acquire_locked_page_cache(page, true);
		if (cache == nullptr)
			continue;

		if (page->State() != PAGE_STATE_ACTIVE) {
			// page is no longer in the cache or in this queue
			cache->ReleaseRefAndUnlock();
			continue;
		}

		if (virtualAccessedBit && page->WiredCount() > 0) {
			// Page is in wired queue. Don't clear "Accessed" bit
			// as it means unmapping the page
			vm_page_requeue(page, true);
			cache->ReleaseRefAndUnlock();
			continue;
		}

		if (page->busy) {
			// page is busy -- requeue at the end
			vm_page_requeue(page, true);
			cache->ReleaseRefAndUnlock();
			continue;
		}

		// Get the page active/modified flags and update the page's usage count.
		// We completely unmap inactive temporary pages. This saves us to
		// iterate through the inactive list as well, since we'll be notified
		// via page fault whenever such an inactive page is used again.
		// We don't remove the mappings of non-temporary pages, since we
		// wouldn't notice when those would become unused and could thus be
		// moved to the cached list.
		int32_t usageCount;
		if (page->WiredCount() > 0 || page->usage_count > 0 || !cache->temporary) {
			usageCount = vm_clear_page_mapping_accessed_flags(page);
		} else
			usageCount = vm_remove_all_page_mappings_if_unaccessed(page);

		if (usageCount > 0) {
			usageCount += page->usage_count + kPageUsageAdvance;
			if (usageCount > kPageUsageMax)
				usageCount = kPageUsageMax;
			// TODO: This would probably also be the place to reclaim swap space.
		} else {
			usageCount += page->usage_count - kPageUsageDecline;
			if (usageCount < 0) {
				usageCount = 0;
				set_page_state(page, PAGE_STATE_INACTIVE);
			}
		}

		page->usage_count = usageCount;

		cache->ReleaseRefAndUnlock();
	}

	remove_page_marker(marker, zone);
}


static void full_scan_inactive_pages(page_stats &pageStats, int32_t despairLevel, MemoryZone &zone) {
	int32_t pagesToFree;
	const bool virtualAccessedBit = VMAddressSpace::Kernel()->TranslationMap()->VirtualAccessedBit();

	pagesToFree = pageStats.unsatisfiedReservations + zone.fFreeOrCachedPagesTarget - (pageStats.totalFreePages + pageStats.cachedPages);

	if (pagesToFree <= 0)
		return;

	uint32_t pagesToModified = 0;

	// Determine how many pages at maximum to send to the modified queue. Since
	// it is relatively expensive to page out pages, we do that on a grander
	// scale only when things get desperate.
	uint32_t maxToFlush = (despairLevel <= 1 ? 32 : 10000);

	struct page marker = {};
	init_page_marker(marker, zone);

	VMPageQueue &queue = sInactivePageQueue;
	InterruptsSpinLocker queueLocker(queue.GetLock());
	uint32_t maxToScan = queue.Count();

	struct page *nextPage = queue.Head();

	while (pagesToFree > 0 && maxToScan > 0) {
		maxToScan--;

		// get the next page
		struct page *page = nextPage;
		if (page == nullptr)
			break;
		nextPage = queue.Next(page);

		if (page->busy)
			continue;

#if defined(CONFIG_MEMORY_HOTPLUG)
		if (page->segment->disconnect_pending)
			continue;
#endif

		// Skip the page is it's wiredand we can't depend on accessed bit
		if (virtualAccessedBit && page->WiredCount() > 0)
			continue;

		// mark the position
		queue.InsertAfter(page, &marker);
		queueLocker.Unlock();

		// lock the page's cache
		VMCache *cache = vm_cache_acquire_locked_page_cache(page, true);
		if (cache == nullptr || page->busy || page->State() != PAGE_STATE_INACTIVE || (virtualAccessedBit && page->WiredCount() > 0)) {
			if (cache != nullptr)
				cache->ReleaseRefAndUnlock();
			queueLocker.Lock();
			nextPage = queue.Next(&marker);
			queue.Remove(&marker);
			continue;
		}

		// Get the accessed count, clear the accessed/modified flags and
		// unmap the page, if it hasn't been accessed.
		int32_t usageCount;
		if (page->WiredCount() > 0)
			usageCount = vm_clear_page_mapping_accessed_flags(page);
		else
			usageCount = vm_remove_all_page_mappings_if_unaccessed(page);

		// update usage count
		if (usageCount > 0) {
			usageCount += page->usage_count + kPageUsageAdvance;
			if (usageCount > kPageUsageMax)
				usageCount = kPageUsageMax;
		} else {
			usageCount += page->usage_count - kPageUsageDecline;
			if (usageCount < 0)
				usageCount = 0;
		}

		page->usage_count = usageCount;

		// Move to fitting queue or requeue:
		// * Active mapped pages go to the active queue.
		// * Inactive mapped (i.e. wired) pages are requeued.
		// * Inactive unmapped pages which are not dirty and can't be written to backing
		//   store remain inactive in order not to remove cloned regions.
		// * The remaining pages are cachable. Thus, if unmodified they go to
		//   the cached queue, otherwise to the modified queue (up to a limit).
		//   Note that until in the idle scanning we don't exempt pages of
		//   temporary caches. Apparently we really need memory, so we better
		//   page out memory as well.
		bool isMapped = page->IsMapped();
		if (usageCount > 0) {
			if (isMapped) {
				set_page_state(page, PAGE_STATE_ACTIVE);
			} else
				vm_page_requeue(page, true);
		} else if (isMapped) {
			vm_page_requeue(page, true);
		} else if (!page->modified) {
			// Verify if we can read data back from the cache
			if (!cache->HasPage(page->cache_offset * PAGE_SIZE)) {
				vm_page_requeue(page, true);
			} else {
				set_page_state(page, PAGE_STATE_CACHED);
				pagesToFree--;
			}
		} else if (maxToFlush > 0) {
			set_page_state(page, PAGE_STATE_MODIFIED);
			maxToFlush--;
			pagesToModified++;
		} else
			vm_page_requeue(page, true);

		cache->ReleaseRefAndUnlock();

		// remove the marker
		queueLocker.Lock();
		nextPage = queue.Next(&marker);
		queue.Remove(&marker);
	}

	queueLocker.Unlock();

	// wake up the page writer, if we tossed it some pages
	if (pagesToModified > 0)
		sPageWriterCondition.WakeUp();
}


static void
full_scan_active_pages(page_stats &pageStats, [[maybe_unused]] int32_t despairLevel, MemoryZone &zone) {
	const bool virtualAccessedBit = VMAddressSpace::Kernel()->TranslationMap()->VirtualAccessedBit();
	struct page marker = {};
	init_page_marker(marker, zone);

	VMPageQueue &queue = sActivePageQueue;
	InterruptsSpinLocker queueLocker(queue.GetLock());
	uint32_t maxToScan = queue.Count();

	int32_t pagesToDeactivate = pageStats.unsatisfiedReservations + zone.fFreeOrCachedPagesTarget - (pageStats.totalFreePages + pageStats.cachedPages) + std::max((int32_t) zone.fInactivePagesTarget - (int32_t) maxToScan, (int32_t) 0);
	if (pagesToDeactivate <= 0)
		return;

	struct page *nextPage = queue.Head();

	while (pagesToDeactivate > 0 && maxToScan > 0) {
		maxToScan--;

		// get the next page
		struct page *page = nextPage;
		if (page == nullptr)
			break;
		nextPage = queue.Next(page);

		if (page->busy)
			continue;

#if defined(CONFIG_MEMORY_HOTPLUG)
		if (page->segment->disconnect_pending)
			continue;
#endif

		if (virtualAccessedBit && page->WiredCount() > 0)
			continue;

		// mark the position
		queue.InsertAfter(page, &marker);
		queueLocker.Unlock();

		// lock the page's cache
		VMCache *cache = vm_cache_acquire_locked_page_cache(page, true);
		if (cache == nullptr || page->busy || page->State() != PAGE_STATE_ACTIVE || (virtualAccessedBit && page->WiredCount() > 0)) {
			if (cache != nullptr)
				cache->ReleaseRefAndUnlock();
			queueLocker.Lock();
			nextPage = queue.Next(&marker);
			queue.Remove(&marker);
			continue;
		}

		// Get the page active/modified flags and update the page's usage count.
		int32_t usageCount = vm_clear_page_mapping_accessed_flags(page);

		if (usageCount > 0) {
			usageCount += page->usage_count + kPageUsageAdvance;
			if (usageCount > kPageUsageMax)
				usageCount = kPageUsageMax;
			// TODO: This would probably also be the place to reclaim swap space.
		} else {
			usageCount += page->usage_count - kPageUsageDecline;
			if (usageCount <= 0) {
				usageCount = 0;
				set_page_state(page, PAGE_STATE_INACTIVE);
			}
		}

		page->usage_count = usageCount;

		cache->ReleaseRefAndUnlock();

		// remove the marker
		queueLocker.Lock();
		nextPage = queue.Next(&marker);
		queue.Remove(&marker);
	}
}


static void
page_daemon_idle_scan(page_stats &pageStats, MemoryZone &zone) {
	// Walk the inactive list and transfer pages to the cached and modified
	// queues.
	if (pageStats.totalFreePages < (int32_t) zone.fFreePagesTarget) {
		// We want more actually free pages, so free some from the cached
		// ones.
		uint32_t freed = free_cached_pages(
		        zone.fFreePagesTarget - pageStats.totalFreePages, false,
		        zone);
		if (freed > 0)
			unreserve_pages_for_zone(freed, zone);
	}

	// Walk the active list and move pages to the inactive queue.
	zone.get_page_stats(&pageStats);
	idle_scan_active_pages(pageStats, zone);
}


static void
page_daemon_full_scan(page_stats &pageStats, int32_t despairLevel, MemoryZone &zone) {
	// Walk the inactive list and transfer pages to the cached and modified
	// queues.
	full_scan_inactive_pages(pageStats, despairLevel, zone);

	// Free cached pages. Also wake up reservation waiters.
	zone.get_page_stats(&pageStats);
	int32_t pagesToFree = pageStats.unsatisfiedReservations + zone.fFreePagesTarget - (pageStats.totalFreePages);
	if (pagesToFree > 0) {
		uint32_t freed = free_cached_pages(pagesToFree, true, zone);
		if (freed > 0)
			unreserve_pages_for_zone(freed, zone);
	}

	// Walk the active list and move pages to the inactive queue.
	zone.get_page_stats(&pageStats);
	full_scan_active_pages(pageStats, despairLevel, zone);
}


static int
page_daemon(void *arg) {
	int32_t despairLevel = 0;
	intptr_t zoneIndex = (intptr_t) arg;

	// Avoid boot time trigger
	snooze(SECONDS(60));

	while (true) {
		sPageDaemonCondition[zoneIndex].ClearActivated();

		if (sMemoryZone[zoneIndex].fTotalPages == 0) {
			sPageDaemonCondition[zoneIndex].Wait(B_INFINITE_TIMEOUT, false);
		}

		// evaluate the free pages situation
		page_stats pageStats;
		sMemoryZone[zoneIndex].update_cumulative_stats();
		sMemoryZone[zoneIndex].get_page_stats(&pageStats);

		if (!do_active_paging(pageStats, sMemoryZone[zoneIndex])) {
			// Things look good -- just maintain statistics and keep the pool
			// of actually free pages full enough.
			despairLevel = 0;
			page_daemon_idle_scan(pageStats, sMemoryZone[zoneIndex]);
			sPageDaemonCondition[zoneIndex].Wait(kIdleScanWaitInterval, false);

#if !defined(CONFIG_NO_TRUSTZONE)
			if(is_ns_irq_pending()) {
				preempt();
			}
#endif
		} else {
			// Not enough free pages. We need to do some real work.
			despairLevel = std::max(despairLevel + 1, (int32_t) 3);
			page_daemon_full_scan(pageStats, despairLevel, sMemoryZone[zoneIndex]);

			// Don't wait after the first full scan, but rather immediately
			// check whether we were successful in freeing enough pages and
			// re-run with increased despair level. The first scan is
			// conservative with respect to moving inactive modified pages to
			// the modified list to avoid thrashing. The second scan, however,
			// will not hold back.
			if (despairLevel > 1)
				snooze(kBusyScanWaitInterval);
		}
	}

	return 0;
}

void task_oom_data::Reset() {
	memory_wait_limit = default_oom_time;
	first_oom = 0;
	oom_count = 0;
	oom_thread_count = 0;
}

time_t task_oom_data::BeginOOM(time_t currentTime) {
	InterruptsSmpSpinLocker locker(lock);
	++oom_thread_count;
	if (oom_thread_count == 1) {
		// Reset max time
		time_taken_max = 0;

		// Check if we need to reset
		if (currentTime - first_oom >= oom_reset_interval) {
			first_oom = currentTime;
			memory_wait_limit = default_oom_time;
			oom_count = 1;
		} else {
			if (oom_count >= oom_max_in_interval) {
				return 0;
			}
			++oom_count;
		}
	}
	return memory_wait_limit;
}

void task_oom_data::EndOOM(time_t time_taken) {
	InterruptsSmpSpinLocker locker(lock);
	// One thread less waiting. Only if there are 0 threads left we update stats
	--oom_thread_count;
	// Use max waiting time of all threads to calculate
	time_taken_max = std::max(time_taken_max, time_taken);
	if (oom_thread_count == 0) {
		if (time_taken_max > memory_wait_limit) {
			memory_wait_limit = 0;
		} else {
			memory_wait_limit -= time_taken_max;
		}
	}
}

/*!	Returns how many pages could *not* be reserved.
*/
static uint32_t reserve_pages(uint32_t count, int priority, bool dontWait,
                              MemoryZone &zone, uint8_t zoneIndex, bool oomKill) {
	const int32_t dontTouch = kPageReserveForPriority[priority];

	while (true) {
		count -= reserve_some_pages_from_zone(count, dontTouch, zone);

		if (count == 0)
			return 0;

		if (__atomic_load_n(&zone.fUnsatisfiedPageReservations, __ATOMIC_RELAXED) == 0) {
			count -= free_cached_pages(count, dontWait, zone);

			if (count == 0)
				return count;
		}

		if (dontWait)
			return count;

		// we need to wait for pages to become available

		MutexLocker pageDeficitLocker(zone.fPageDeficitLock);

		bool notifyDaemon =
		        __atomic_fetch_add(&zone.fUnsatisfiedPageReservations,
		                           count, __ATOMIC_RELAXED) == 0;

		if (__atomic_load_n(&zone.fUnreservedFreePages, __ATOMIC_RELAXED) > dontTouch) {
			// the situation changed
			__atomic_fetch_sub(&zone.fUnsatisfiedPageReservations, count, __ATOMIC_RELAXED);
			continue;
		}

		PageReservationWaiter waiter;
		waiter.dontTouch = dontTouch;
		waiter.missing = count;
		waiter.threadPriority = get_current_thread()->priority;
		waiter.queued = true;

		// insert ordered (i.e. after all waiters with higher or equal priority)
		PageReservationWaiter *otherWaiter = nullptr;
		for (PageReservationWaiterList::Iterator it = zone.fPageReservationWaiters.GetIterator();
		     (otherWaiter = it.Next()) != nullptr;) {
			if (waiter < *otherWaiter)
				break;
		}

		task_oom_data *oom_data = &get_current_thread()->task->oom_data;
		time_t waitStart = system_time();
		time_t timeout = oom_data->BeginOOM(waitStart);

		zone.fPageReservationWaiters.InsertBefore(otherWaiter, &waiter);

		cond_var_entry waitEntry;
		waiter.cond.Add(&waitEntry);

		if (notifyDaemon)
			sPageDaemonCondition[zoneIndex].WakeUp();


		pageDeficitLocker.Unlock();

		low_resource(B_KERNEL_RESOURCE_PAGES, count, B_RELATIVE_TIMEOUT, 0);

#ifdef DEBUG_OOM
		printk(KERN_ERR | WITHUART, "Wait %" PRId64 " msec %d free\n", timeout / MILLISECS(1),
		       zone.fUnreservedFreePages);
#endif

		waitEntry.Wait(B_RELATIVE_TIMEOUT, timeout);

#ifdef DEBUG_OOM
		printk(KERN_ERR | WITHUART, "Waited %" PRId64 " msec, %d free, %" PRId64 " wait limit\n", (system_time() - waitStart) / MILLISECS(1),
		       zone.fUnreservedFreePages, timeout);
#endif

		oom_data->EndOOM(system_time() - waitStart);

		pageDeficitLocker.Lock();

		if (!waiter.queued) {
			// We might still have succeeded acquiring the memory
			return 0;
		}

		zone.fPageReservationWaiters.Remove(&waiter);

		// Task has been killed, don't wait for memory anymore
		__atomic_fetch_sub(&zone.fUnsatisfiedPageReservations, waiter.missing, __ATOMIC_RELAXED);

		// Retry if OOM killer reclaimed some memory
		// User allocations are killable - SIGKILL on the process will
		// not retry waiting.
		if (oomKill && oom_kill_page_alloc_retry(
		                       priority == GFP_PRIO_USER || priority == GFP_PRIO_EXTERNAL)) {
			// Update remaining count to reserve
			count = waiter.missing;
			continue;
		}

		return waiter.missing;
	}
}


//	#pragma mark - private kernel API


/*!	Writes a range of modified pages of a cache to disk.
	You need to hold the VMCache lock when calling this function.
	Note that the cache lock is released in this function.
	\param cache The cache.
	\param firstPage Offset (in page size units) of the first page in the range.
	\param endPage End offset (in page size units) of the page range. The page
		at this offset is not included.
*/
int vm_page_write_modified_page_range(struct VMCache *cache, off_t firstPage,
                                      off_t endPage) {
	static const int32_t kMaxPages = 256;
	int32_t maxPages = cache->MaxPagesPerWrite();
	if (maxPages < 0 || maxPages > kMaxPages)
		maxPages = kMaxPages;

	const uint32_t allocationFlags = CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL;

	PageWriteWrapper stackWrappersPool[2];
	PageWriteWrapper *stackWrappers[1];
	PageWriteWrapper *wrapperPool = new (std::nothrow, wait_flags_t(allocationFlags)) PageWriteWrapper[maxPages + 1];
	PageWriteWrapper **wrappers = new (std::nothrow, wait_flags_t(allocationFlags)) PageWriteWrapper *[maxPages];
	if (wrapperPool == nullptr || wrappers == nullptr) {
		// don't fail, just limit our capabilities
		delete[] wrapperPool;
		delete[] wrappers;
		wrapperPool = stackWrappersPool;
		wrappers = stackWrappers;
		maxPages = 1;
	}

	int32_t nextWrapper = 0;
	int32_t usedWrappers = 0;

	PageWriteTransfer transfer;
	bool transferEmpty = true;

	VMCachePagesTree::Iterator it = cache->pages.GetIterator(firstPage, true, true);

	while (true) {
		struct page *page = it.Next();
		if (page == nullptr || page->cache_offset >= (off_t) endPage) {
			if (transferEmpty)
				break;

			page = nullptr;
		}

		if (page != nullptr) {
			if (page->busy || (page->State() != PAGE_STATE_MODIFIED && !vm_test_map_modification(page))) {
				page = nullptr;
			}
		}

		PageWriteWrapper *wrapper = nullptr;
		if (page != nullptr) {
			wrapper = &wrapperPool[nextWrapper++];
			if (nextWrapper > maxPages)
				nextWrapper = 0;

			wrapper->SetTo(page);

			if (transferEmpty || transfer.AddPage(page)) {
				if (transferEmpty) {
					transfer.SetTo(nullptr, page, maxPages);
					transferEmpty = false;
				}

				wrappers[usedWrappers++] = wrapper;
				continue;
			}
		}

		if (transferEmpty)
			continue;

		cache->Unlock();
		int status = transfer.Schedule(0);
		cache->Lock();

		for (int32_t i = 0; i < usedWrappers; i++)
			wrappers[i]->Done(status);

		usedWrappers = 0;

		if (page != nullptr) {
			transfer.SetTo(nullptr, page, maxPages);
			wrappers[usedWrappers++] = wrapper;
		} else
			transferEmpty = true;
	}

	if (wrapperPool != stackWrappersPool) {
		delete[] wrapperPool;
		delete[] wrappers;
	}

	return 0;
}

/*!	You need to hold the VMCache lock when calling this function.
	Note that the cache lock is released in this function.
*/
int vm_page_write_modified_pages(VMCache *cache) {
	return vm_page_write_modified_page_range(cache, 0,
	                                         (cache->virtual_end + PAGE_SIZE - 1) >> PAGE_SHIFT);
}


/*!	Schedules the page writer to write back the specified \a page.
	Note, however, that it might not do this immediately, and it can well
	take several seconds until the page is actually written out.
*/
void vm_page_schedule_write_page(struct page *page) {
	ASSERT(page->State() == PAGE_STATE_MODIFIED);
	vm_page_requeue(page, false);
	sPageWriterCondition.WakeUp();
}

/*!	Cache must be locked.
*/
void vm_page_schedule_write_page_range(struct VMCache *cache, uint32_t firstPage,
                                       uint32_t endPage) {
	uint32_t modified = 0;
	for (VMCachePagesTree::Iterator it = cache->pages.GetIterator(firstPage, true, true);
	     struct page *page = it.Next();) {
		if (page->cache_offset >= (off_t) endPage)
			break;

		if (!page->busy && page->State() == PAGE_STATE_MODIFIED) {
			vm_page_requeue(page, false);
			modified++;
		}
	}

	if (modified > 0)
		sPageWriterCondition.WakeUp();
}

void vm_page_unreserve_pages(vm_reservation_t *reservation) {
#if defined(CONFIG_MEMORY_HOTPLUG)
	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		uint32_t count = reservation->counts[i];
		reservation->counts[i] = 0;

		if (count > 0) {
			unreserve_pages_for_zone(count, sMemoryZone[i]);
		}
	}
#else
	uint32_t count = reservation->count;
	reservation->count = 0;

	if (count == 0)
		return;

	unreserve_pages_for_zone(count, sMemoryZone[0]);
#endif
}

int vm_reserve_pages(vm_reservation_t *reservation,
                     uint32_t count, int priority, bool dontWait,
                     bool oomKill) {
	struct thread *th = thread_get_current_thread();
	uint32_t remaining;

	if (th == nullptr || get_preempt_count()) {
		dontWait = true;
	}

#if !defined(CONFIG_MEMORY_HOTPLUG)
	reservation->count = 0;
#else
	reservation->counts[0] = 0;
	reservation->counts[1] = 0;
#endif

	if (count == 0) {
		return true;
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	if (priority == GFP_PRIO_EXTERNAL && !sDynamicMemoryDriver) {
		priority = GFP_PRIO_USER;
	}
#else
	if (priority == GFP_PRIO_EXTERNAL) {
		priority = GFP_PRIO_USER;
	}
#endif

#if defined(CONFIG_MEMORY_HOTPLUG)
	if (priority == GFP_PRIO_EXTERNAL && sDynamicMemoryDriver && sMemoryZone[TASK_ZONE_EXTERNAL].fTotalPages > 0) {
		// First try allocation from external zone
		// without waiting
		remaining = reserve_pages(count, priority, true,
		                          sMemoryZone[TASK_ZONE_EXTERNAL], 1, false);

		if (remaining == 0) {
			reservation->counts[TASK_ZONE_EXTERNAL] = count;
			return true;
		}

		unreserve_pages_for_zone(count - remaining, sMemoryZone[1]);

		if (!dontWait) {
			// Try allocation from internal zone without waiting
			remaining = reserve_pages(count, priority, true,
			                          sMemoryZone[TASK_ZONE_INTERNAL], TASK_ZONE_INTERNAL,
			                          false);

			if (remaining == 0) {
				reservation->counts[TASK_ZONE_INTERNAL] = count;
				return true;
			}

			unreserve_pages_for_zone(count - remaining,
			                         sMemoryZone[TASK_ZONE_INTERNAL]);

			// Try allocation from external zone with waiting
			remaining = reserve_pages(count, priority, false,
			                          sMemoryZone[TASK_ZONE_EXTERNAL],
			                          TASK_ZONE_EXTERNAL, oomKill);

			if (remaining == 0) {
				reservation->counts[TASK_ZONE_EXTERNAL] = count;
				return true;
			}

			unreserve_pages_for_zone(count - remaining,
			                         sMemoryZone[TASK_ZONE_EXTERNAL]);
		}
	}
#endif

	remaining = reserve_pages(count, priority, dontWait,
	                          sMemoryZone[TASK_ZONE_INTERNAL], TASK_ZONE_INTERNAL,
	                          oomKill);

	if (remaining == 0) {
#if !defined(CONFIG_MEMORY_HOTPLUG)
		reservation->count = count;
#else
		reservation->counts[TASK_ZONE_INTERNAL] = count;
#endif
		return true;
	}

	unreserve_pages_for_zone(count - remaining,
	                         sMemoryZone[TASK_ZONE_INTERNAL]);

	return false;
}

#if defined(CONFIG_MEMORY_HOTPLUG)
static int vm_page_try_reserve_pages(vm_reservation_t *reservation,
                                     uint32_t count, int priority) {
	return vm_reserve_pages(reservation, count, priority, true, false);
}
#endif

struct page *vm_page_allocate_page_etc(vm_reservation_t *reservation, uint32_t flags) {
	uint32_t pageState = flags & VM_PAGE_ALLOC_STATE;

	ASSERT(pageState != PAGE_STATE_FREE);
	ASSERT(pageState != PAGE_STATE_CLEAR);

#if !defined(CONFIG_MEMORY_HOTPLUG)
	ASSERT(reservation->count > 0);
	reservation->count--;
#else
	int zoneIndex;

	if (reservation->counts[0] > 0) {
		zoneIndex = 0;
		--reservation->counts[0];
	} else if (reservation->counts[1] > 0) {
		zoneIndex = 1;
		--reservation->counts[1];
	} else {
		panic("page reservation is empty");
		return NULL;
	}
#endif

	VMPageQueue *queue;
	VMPageQueue *otherQueue;

#if defined(CONFIG_MEMORY_HOTPLUG)
	MemoryZone *zone = &sMemoryZone[zoneIndex];
#else
	MemoryZone *zone = &sMemoryZone[0];
#endif

	if ((flags & VM_PAGE_ALLOC_CLEAR) != 0) {
		queue = &zone->fClearPageQueue;
		otherQueue = &zone->fFreePageQueue;
	} else {
		queue = &zone->fFreePageQueue;
		otherQueue = &zone->fClearPageQueue;
	}

	ReadLocker locker(zone->fFreePageQueuesLock);

	struct page *page = queue->RemoveHeadUnlocked();
	if (page == nullptr) {
		// if the primary queue was empty, grab the page from the
		// secondary queue
		page = otherQueue->RemoveHeadUnlocked();

		if (page == nullptr) {
			// Unlikely, but possible: the page we have reserved has moved
			// between the queues after we checked the first queue. Grab the
			// write locker to make sure this doesn't happen again.
			locker.Unlock();
			WriteLocker writeLocker(zone->fFreePageQueuesLock);

			page = queue->RemoveHead();

			if (page == nullptr)
				page = otherQueue->RemoveHead();

			if (page == nullptr) {
				writeLocker.Unlock();
				panic("Had reserved page, but there is none!");
			}

			// downgrade to read lock
			locker.Lock();
		}
	}

	if (page->CacheRef() != nullptr)
		panic("supposed to be free page %p has cache\n", page);

	int oldPageState = page->State();
	page->SetState(pageState);
	page->busy = (flags & VM_PAGE_ALLOC_BUSY) != 0;
	page->usage_count = 0;
	page->accessed = false;
	page->modified = false;

	ASSERT(!page->wired_count);

	locker.Unlock();

	if (pageState < PAGE_STATE_FIRST_UNQUEUED) {
		switch (pageState) {
			case PAGE_STATE_ACTIVE:
				sActivePageQueue.AppendUnlocked(page);
				break;
			case PAGE_STATE_INACTIVE:
				sInactivePageQueue.AppendUnlocked(page);
				break;
			case PAGE_STATE_MODIFIED:
				sModifiedPageQueue.AppendUnlocked(page);
				break;
			case PAGE_STATE_CACHED:
				zone->fCachedPageQueue.AppendUnlocked(page);
				break;
			default:
				panic("Invalid state");
		}
	}

	// clear the page, if we had to take it from the free queue and a clear
	// page was requested
	if ((flags & VM_PAGE_ALLOC_CLEAR) != 0 && oldPageState != PAGE_STATE_CLEAR)
		clear_page(page);

	return page;
}

static void allocate_page_run_cleanup(VMPageQueue::PageList &freePages,
                                      VMPageQueue::PageList &clearPages, MemoryZone &zone) {
	while (struct page *page = freePages.RemoveHead()) {
		page->busy = false;
		page->SetState(PAGE_STATE_FREE);
		zone.fFreePageQueue.PrependUnlocked(page);
	}

	while (struct page *page = clearPages.RemoveHead()) {
		page->busy = false;
		page->SetState(PAGE_STATE_CLEAR);
		zone.fClearPageQueue.PrependUnlocked(page);
	}

	sFreePageCondition.NotifyAll();
}


/*!	Tries to allocate the a contiguous run of \a length pages starting at
	index \a start.

	The caller must have write-locked the free/clear page queues. The function
	will unlock regardless of whether it succeeds or fails.

	If the function fails, it cleans up after itself, i.e. it will free all
	pages it managed to allocate.

	\param start The start index (into \c sPages) of the run.
	\param length The number of pages to allocate.
	\param flags Page allocation flags. Encodes the state the function shall
		set the allocated pages to, whether the pages shall be marked busy
		(VM_PAGE_ALLOC_BUSY), and whether the pages shall be cleared
		(VM_PAGE_ALLOC_CLEAR).
	\param freeClearQueueLocker Locked WriteLocker for the free/clear page
		queues in locked state. Will be unlocked by the function.
	\return The index of the first page that could not be allocated. \a length
		is returned when the function was successful.
*/
static vm_pindex_t allocate_page_run(MemorySegment *segment, vm_pindex_t start,
                                     vm_pindex_t length, uint32_t flags, WriteLocker &freeClearQueueLocker,
                                     MemoryZone &zone) {
	uint32_t pageState = flags & VM_PAGE_ALLOC_STATE;
	ASSERT(pageState != PAGE_STATE_FREE);
	ASSERT(pageState != PAGE_STATE_CLEAR);

	if (start + length > segment->end_pfn)
		return 0;

	// Pull the free/clear pages out of their respective queues. Cached pages
	// are allocated later.
	vm_pindex_t cachedPages = 0;
	VMPageQueue::PageList freePages;
	VMPageQueue::PageList clearPages;
	vm_pindex_t i = 0;

	for (; i < length; i++) {
		bool pageAllocated = true;
		bool noPage = false;
		struct page &page = segment->pages[start + i];
		switch (page.State()) {
			case PAGE_STATE_CLEAR:
				zone.fClearPageQueue.Remove(&page);
				clearPages.Add(&page);
				break;
			case PAGE_STATE_FREE:
				zone.fFreePageQueue.Remove(&page);
				freePages.Add(&page);
				break;
			case PAGE_STATE_CACHED:
				// We allocate cached pages later.
				cachedPages++;
				pageAllocated = false;
				break;

			default:
				// Probably a page was cached when our caller checked. Now it's
				// gone and we have to abort.
				noPage = true;
				break;
		}

		if (noPage)
			break;

		if (pageAllocated) {
			page.SetState(flags & VM_PAGE_ALLOC_STATE);
			page.busy = (flags & VM_PAGE_ALLOC_BUSY) != 0;
			page.usage_count = 0;
			page.accessed = false;
			page.modified = false;
		}
	}

	if (i < length) {
		// failed to allocate a page -- free all that we've got
		allocate_page_run_cleanup(freePages, clearPages, zone);
		return i;
	}

	freeClearQueueLocker.Unlock();

	if (cachedPages > 0) {
		// allocate the pages that weren't free but cached
		vm_pindex_t freedCachedPages = 0;
		vm_pindex_t nextIndex = start;
		struct page *freePage = freePages.Head();
		struct page *clearPage = clearPages.Head();
		while (cachedPages > 0) {
			// skip, if we've already got the page
			if (freePage != nullptr && size_t(freePage - segment->pages) == nextIndex) {
				freePage = freePages.GetNext(freePage);
				nextIndex++;
				continue;
			}
			if (clearPage != nullptr && size_t(clearPage - segment->pages) == nextIndex) {
				clearPage = clearPages.GetNext(clearPage);
				nextIndex++;
				continue;
			}

			// free the page, if it is still cached
			struct page &page = segment->pages[nextIndex];
			if (!free_cached_page(&page, false, zone)) {
				// TODO: if the page turns out to have been freed already,
				// there would be no need to fail
				break;
			}

			page.SetState(flags & VM_PAGE_ALLOC_STATE);
			page.busy = (flags & VM_PAGE_ALLOC_BUSY) != 0;
			page.usage_count = 0;
			page.accessed = false;
			page.modified = false;

			freePages.InsertBefore(freePage, &page);
			freedCachedPages++;
			cachedPages--;
			nextIndex++;
		}

		// If we have freed cached pages, we need to balance things.
		if (freedCachedPages > 0)
			unreserve_pages_for_zone(freedCachedPages, zone);

		if (nextIndex - start < length) {
			// failed to allocate all cached pages -- free all that we've got
			freeClearQueueLocker.Lock();
			allocate_page_run_cleanup(freePages, clearPages, zone);
			freeClearQueueLocker.Unlock();

			return nextIndex - start;
		}
	}

	// clear pages, if requested
	if ((flags & VM_PAGE_ALLOC_CLEAR) != 0) {
		for (VMPageQueue::PageList::Iterator it = freePages.GetIterator();
		     struct page *page = it.Next();) {
			clear_page(page);
		}
	}

	// add pages to target queue
	if (pageState < PAGE_STATE_FIRST_UNQUEUED) {
		freePages.MoveFrom(&clearPages);

		switch (pageState) {
			case PAGE_STATE_ACTIVE:
				sActivePageQueue.AppendUnlocked(freePages, length);
				break;
			case PAGE_STATE_INACTIVE:
				sInactivePageQueue.AppendUnlocked(freePages, length);
				break;
			case PAGE_STATE_MODIFIED:
				sModifiedPageQueue.AppendUnlocked(freePages, length);
				break;
			case PAGE_STATE_CACHED:
				zone.fCachedPageQueue.AppendUnlocked(freePages, length);
				break;
			default:
				panic("Invalid page state");
		}
	}

	return length;
}

vm_page_t vm_page_allocate_page_run(uint32_t flags,
                                    size_t length,
                                    const physical_address_restrictions *restrictions,
                                    [[maybe_unused]] int priority,
                                    vm_reservation_t *reservation) {
#if defined(CONFIG_MEMORY_HOTPLUG)
	ReadLocker pageSegmentsReadLocker(sPageSegmentsRWLock);
#endif

	for (int index = 0; index < sPageSegmentCount; ++index) {
		if (!sPageSegments[index])
			continue;

#if defined(CONFIG_MEMORY_HOTPLUG)
		MemoryZone *zone = &sMemoryZone[sPageSegments[index]->zone_index];

		if (reservation->counts[sPageSegments[index]->zone_index] < length)
			continue;
#else
		MemoryZone *zone = &sMemoryZone[0];
#endif

		// compute start and end page index
		vm_pindex_t requestedStart = std::max<vm_pindex_t>(restrictions->low_address / PAGE_SIZE,
		                                                   sPageSegments[index]->base_pfn) -
		                             sPageSegments[index]->base_pfn;
		vm_pindex_t start = requestedStart;
		vm_pindex_t end;
		if (restrictions->high_address > 0) {
			end = std::max<vm_pindex_t>(restrictions->high_address / PAGE_SIZE,
			                            sPageSegments[index]->base_pfn) -
			      sPageSegments[index]->base_pfn;
			end = std::min(end, sPageSegments[index]->end_pfn - sPageSegments[index]->base_pfn);
		} else
			end = sPageSegments[index]->end_pfn - sPageSegments[index]->base_pfn;

		// compute alignment mask
		vm_pindex_t alignmentMask = std::max<vm_pindex_t>(restrictions->alignment / PAGE_SIZE,
		                                                  (vm_paddr_t) 1) -
		                            1;

		ASSERT(((alignmentMask + 1) & alignmentMask) == 0);
		// alignment must be a power of 2

		// compute the boundary mask
		uint32_t boundaryMask = 0;
		if (restrictions->boundary != 0) {
			vm_pindex_t boundary = restrictions->boundary / PAGE_SIZE;
			// boundary must be a power of two and not less than alignment and
			// length
			ASSERT(((boundary - 1) & boundary) == 0);
			ASSERT(boundary >= alignmentMask + 1);
			ASSERT(boundary >= length);

			boundaryMask = -boundary;
		}

		WriteLocker freeClearQueueLocker(zone->fFreePageQueuesLock);

		// First we try to get a run with free pages only. If that fails, we also
		// consider cached pages. If there are only few free pages and many cached
		// ones, the odds are that we won't find enough contiguous ones, so we skip
		// the first iteration in this case.
		int32_t freePages = __atomic_load_n(&zone->fUnreservedFreePages, __ATOMIC_RELAXED);
		size_t useCached =
		        freePages > 0 && (vm_pindex_t) freePages > 2 * (vm_pindex_t) length ? 0 : 1;

		for (;;) {
			if (alignmentMask != 0 || boundaryMask != 0) {
				vm_pindex_t offsetStart = start + sPageSegments[index]->base_pfn;

				// enforce alignment
				if ((offsetStart & alignmentMask) != 0)
					offsetStart = (offsetStart + alignmentMask) & ~alignmentMask;

				// enforce boundary
				if (boundaryMask != 0 && ((offsetStart ^ (offsetStart + length - 1)) & boundaryMask) != 0) {
					offsetStart = (offsetStart + length - 1) & boundaryMask;
				}

				start = offsetStart - sPageSegments[index]->base_pfn;
			}

			if (start + length > end) {
				if (useCached == 0) {
					// The first iteration with free pages only was unsuccessful.
					// Try again also considering cached pages.
					useCached = 1;
					start = requestedStart;
					continue;
				}

				freeClearQueueLocker.Unlock();
				return nullptr;
			}

			bool foundRun = true;
			vm_pindex_t i;
			for (i = 0; i < length; i++) {
				uint32_t pageState = sPageSegments[index]->pages[start + i].State();
				if (pageState != PAGE_STATE_FREE && pageState != PAGE_STATE_CLEAR && (pageState != PAGE_STATE_CACHED || useCached == 0)) {
					foundRun = false;
					break;
				}
			}

			if (foundRun) {
				i = allocate_page_run(sPageSegments[index], start, length, flags, freeClearQueueLocker, *zone);
				if (i == length) {
#if defined(CONFIG_MEMORY_HOTPLUG)
					reservation->counts[sPageSegments[index]->zone_index] -= length;
#else
					reservation->count -= length;
#endif

					return &sPageSegments[index]->pages[start];
				}

				// apparently a cached page couldn't be allocated -- skip it and
				// continue
				freeClearQueueLocker.Lock();
			}

			start += i + 1;
		}
	}

	return nullptr;
}

void vm_page_free_etc(VMCache *cache, struct page *page, vm_reservation_t *reservation) {
	ASSERT(page->State() != PAGE_STATE_FREE && page->State() != PAGE_STATE_CLEAR);

	int zoneIndex = page->Zone();
	MemoryZone *zone = &sMemoryZone[zoneIndex];

	if (page->State() == PAGE_STATE_MODIFIED && cache->temporary)
		__atomic_fetch_sub(&sModifiedTemporaryPages, 1, __ATOMIC_RELAXED);

	free_page(page, false);
	if (reservation == nullptr) {
		unreserve_pages_for_zone(1, *zone);
	} else {
#if !defined(CONFIG_MEMORY_HOTPLUG)
		++reservation->count;
#else
		++reservation->counts[zoneIndex];
#endif
	}
}

void vm_page_set_state(struct page *page, int pageState) {
	ASSERT(page->State() != PAGE_STATE_FREE && page->State() != PAGE_STATE_CLEAR);

	if (pageState == PAGE_STATE_FREE || pageState == PAGE_STATE_CLEAR) {
		int zoneIndex = page->Zone();
		MemoryZone *zone = &sMemoryZone[zoneIndex];
		free_page(page, pageState == PAGE_STATE_CLEAR);
		unreserve_pages_for_zone(1, *zone);
	} else {
		set_page_state(page, pageState);
	}
}

void vm_page_requeue(struct page *page, bool tail) {
	ASSERT(page->Cache());
	page->Cache()->AssertLocked();

	int zoneIndex = page->Zone();
	MemoryZone *zone = &sMemoryZone[zoneIndex];

	// DEBUG_PAGE_ACCESS_CHECK(page);
	// TODO: This assertion cannot be satisfied by idle_scan_active_pages()
	// when it requeues busy pages. The reason is that vm_soft_fault()
	// (respectively fault_get_page()) and the file cache keep newly
	// allocated pages accessed while they are reading them from disk. It
	// would probably be better to change that code and reenable this
	// check.

	VMPageQueue *queue = nullptr;

	switch (page->State()) {
		case PAGE_STATE_ACTIVE:
			queue = &sActivePageQueue;
			break;
		case PAGE_STATE_INACTIVE:
			queue = &sInactivePageQueue;
			break;
		case PAGE_STATE_MODIFIED:
			queue = &sModifiedPageQueue;
			break;
		case PAGE_STATE_CACHED:
			queue = &zone->fCachedPageQueue;
			break;
		case PAGE_STATE_FREE:
		case PAGE_STATE_CLEAR:
			panic("vm_page_requeue() called for free/clear page %p", page);
		case PAGE_STATE_WIRED:
		case PAGE_STATE_UNUSED:
			return;
		default:
			panic("vm_page_touch: vm_page %p in invalid state %d\n",
			      page, page->State());
	}

	queue->RequeueUnlocked(page, tail);
}

static vm_page_t pfn_to_page_locked(vm_pindex_t pageNumber) {
	for (int index = 0; index < sPageSegmentCount; ++index) {
		if (!sPageSegments[index])
			continue;
		if (pageNumber >= sPageSegments[index]->base_pfn && pageNumber < sPageSegments[index]->end_pfn) {
			return &sPageSegments[index]->pages[pageNumber - sPageSegments[index]->base_pfn];
		}
	}

	return nullptr;
}

vm_page_t pfn_to_page(vm_pindex_t pageNumber) {
#if defined(CONFIG_MEMORY_HOTPLUG)
	InterruptsSmpReadSpinLocker readLocker(sPageSegmentsRWSpinLock);
#endif

	return pfn_to_page_locked(pageNumber);
}

static void recalculate_free_targets(MemoryZone &zone) {
	// The target of actually free pages. This must be at least the system
	// reserve, but should be a few more pages, so we don't have to extract
	// a cached page with each allocation.
	zone.fFreePagesTarget = VM_PAGE_RESERVE_USER + MAX(32, zone.fTotalPages / 1024);

	// The target of free + cached and inactive pages. On low-memory machines
	// keep things tight. free + cached is the pool of immediately allocatable
	// pages. We want a few inactive pages, so when we're actually paging, we
	// have a reasonably large set of pages to work with.
	if (__atomic_load_n(&zone.fUnreservedFreePages, __ATOMIC_RELAXED) < 16 * 1024) {
		zone.fFreeOrCachedPagesTarget = zone.fFreePagesTarget + 128;
		zone.fInactivePagesTarget = zone.fFreePagesTarget / 3;
	} else {
		zone.fFreeOrCachedPagesTarget = 2 * zone.fFreePagesTarget;
		zone.fInactivePagesTarget = zone.fFreePagesTarget / 2;
	}
}

int vm_attach_memory_etc(vm_paddr_t phys, size_t size, int zone_index, [[maybe_unused]] void *driver_tag, [[maybe_unused]] uint32_t ns_tag) {
	size_t pages_size;
	int i;
	int segment_index = -1;
	struct MemorySegment *seg;
	struct page *page_array;
	void *pages_address;
	char area_name[B_OS_NAME_LENGTH];
	size_t num_pages;

	if (zone_index >= VM_ZONE_COUNT) {
		printk(KERN_ERR | WITHUART, "Invalid zone id %d\n", zone_index);
		return -EINVAL;
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	// Allow only one instance of hotplug operation
	// at a time
	MutexLocker hotplugMutex(sMemoryHotplugMutex);
#endif

#if defined(DEBUG_MEMORY_HOTPLUG)
	printk(KERN_ERR | WITHUART, "Attach memory %" B_PRIxPHYSADDR " -- %" B_PRIxPHYSADDR "\n",
	       phys, phys + size);
#endif

	seg = new (std::nothrow) MemorySegment();

	if (!seg)
		return -ENOMEM;

	ObjectDeleter<MemorySegment> segmentDeleter(seg);

	pages_size = (size >> PAGE_SHIFT) * sizeof(struct page);
	pages_size = roundup2(pages_size, PAGE_SIZE);

	if (pages_size * 2 >= size) {
		// Don't bother attaching tiny areas
		return -EINVAL;
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	if (zone_index == TASK_ZONE_INTERNAL)
#endif
	{
		snprintf(area_name, sizeof(area_name), "secure_pages_%" PRIx64, (uint64_t) phys);
	}
#if defined(CONFIG_MEMORY_HOTPLUG)
	else {
		snprintf(area_name, sizeof(area_name), "ext_pages_%" PRIx64, (uint64_t) phys);
	}
#endif

	vm_paddr_t pagesPhysicalAddress = phys + size - pages_size;

	area_id pages_area_id = map_physical_memory(area_name, &pages_address,
	                                            B_RANDOMIZED_ANY_ADDRESS, pages_size,
	                                            B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, pagesPhysicalAddress);

	if (pages_area_id < 0) {
		return pages_area_id;
	}

	num_pages = (size - pages_size) >> PAGE_SHIFT;
	page_array = (struct page *) pages_address;
	memset(page_array, 0, pages_size);

	// Prevent any optimizations
	__asm__ __volatile__("" ::
	                             : "memory");

	seg->base_pfn = phys >> PAGE_SHIFT;
	seg->end_pfn = pagesPhysicalAddress >> PAGE_SHIFT;
	seg->base_phys = phys;
	seg->end_phys = pagesPhysicalAddress;
	seg->orig_size = size;
	seg->pages = page_array;

#if defined(CONFIG_MEMORY_HOTPLUG)
	seg->dynamic_tag = driver_tag;
	seg->nonsecure_tag = ns_tag;
	seg->pages_area_id = pages_area_id;
	seg->zone_index = zone_index;
#endif

	for (size_t pageIndex = 0; pageIndex < num_pages; ++pageIndex) {
		struct page *page = new (&page_array[pageIndex]) struct page();
		page->Init(pageIndex + seg->base_pfn);
#if defined(CONFIG_MEMORY_HOTPLUG)
		page->segment = seg;
		page->zone_index = zone_index;
#endif
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	WriteLocker pageSegmentsWriteLocker(sPageSegmentsRWLock);
	InterruptsSmpWriteSpinLocker pageSegmentsWriteSpinLocker(sPageSegmentsRWSpinLock);
#endif

	for (i = 0; i < MAX_MEMORY_SEGMENTS; ++i) {
		if (!sPageSegments[i]) {
			segment_index = i;
			break;
		}
	}

	if (segment_index >= 0) {
		// Check for overlaps
		for (i = 0; i < sPageSegmentCount; ++i) {
			if (i == segment_index || !sPageSegments[i])
				continue;
			if (MAX(sPageSegments[i]->base_phys,
			        phys) <
			    MIN(sPageSegments[i]->base_phys + sPageSegments[i]->orig_size, phys + size)) {
				segment_index = -1;
				break;
			}
		}
	}

	if (segment_index < 0) {
#if defined(CONFIG_MEMORY_HOTPLUG)
		pageSegmentsWriteSpinLocker.Unlock();
		pageSegmentsWriteLocker.Unlock();
#endif

		// Wipe all "pages" not to leave any stray data
		memset(page_array, 0xcc, pages_size);

		// Write back and invalidate D-cache to avoid
		// spurious accesses if we change security state
		// of memory
		dcache_wbinv_poc((uintptr_t) page_array, pagesPhysicalAddress,
		                 pages_size);

		// All segments used. Can't attach memory anymore
		delete_area(pages_area_id);
		return -E2BIG;
	}

	sPageSegmentCount = MAX(sPageSegmentCount, segment_index + 1);
	sMemoryZone[zone_index].fTotalPages += num_pages;
	sPageSegments[segment_index] = segmentDeleter.Detach();

#if defined(CONFIG_MEMORY_HOTPLUG)
	// It's safe to drop the hotplug spinlock now
	pageSegmentsWriteSpinLocker.Unlock();
	pageSegmentsWriteLocker.Unlock();
#endif

#if defined(DEBUG_MEMORY_HOTPLUG)
	printk(KERN_ERR | WITHUART, "%zd pages added to zone %d\n", num_pages, zone_index);
#endif

	// Add free pages to the pool
	{
		VMPageQueue::PageList list;

		// Build single list
		for (size_t pageIndex = 0; pageIndex < num_pages; ++pageIndex) {
			list.Add(&page_array[pageIndex]);
		}

		sMemoryZone[zone_index].fFreePageQueue.AppendUnlocked(list, num_pages);
	}

	// Give pages back to the main pool
	unreserve_pages_for_zone(num_pages, sMemoryZone[zone_index]);

	// Unreserve memory to allow tasks to use it
	{
		MutexLocker locker(sAvailableMemoryLock);

		sAvailableMemory += num_pages << PAGE_SHIFT;
		sTotalMemory += num_pages << PAGE_SHIFT;

		// Recalculate sizes to free
		recalculate_free_targets(sMemoryZone[zone_index]);
	}

#if defined(CONFIG_MEMORY_HOTPLUG)
	if (zone_index == TASK_ZONE_EXTERNAL) {
		sPageDaemonCondition[zone_index].WakeUp();
	}
#endif

	sFreePageCondition.NotifyAll();

	return 0;
}

int vm_attach_memory(vm_paddr_t phys, size_t size) {
	return vm_attach_memory_etc(phys, size, TASK_ZONE_INTERNAL, nullptr, 0);
}

#if defined(CONFIG_MEMORY_HOTPLUG)
static bool try_migrate_page_to_internal_zone(VMCache *cache,
                                              struct page *oldPage,
                                              VMPageQueue &pageQueue,
                                              uint32_t &numberOfPagesForMigration,
                                              vm_reservation_t &reservation) {
	struct page *newPage;

	cache->AssertLocked();
	KASSERT_ALWAYS(!oldPage->busy, ("Page can't be busy"));
	KASSERT_ALWAYS(!oldPage->IsMapped(), ("Page can't be mapped"));

	if (numberOfPagesForMigration == 0)
		return false;

	// Mark old page as busy so nobody else can touch it
	oldPage->busy = 1;

	// Release the cache lock temporarily as we don't need it
	cache->Unlock();

	--numberOfPagesForMigration;

	// Allocate new busy page
	newPage = vm_page_allocate_page_etc(&reservation,
	                                    VM_PAGE_ALLOC_BUSY | oldPage->State());

	// Can't possibly fail
	ASSERT(newPage);

	// Copy accessed and modified flags
	newPage->accessed = oldPage->accessed;
	newPage->modified = oldPage->modified;
	newPage->usage_count = 0;

	// Copy page contents
	vm_memcpy_physical_page(page_to_phys(newPage), page_to_phys(oldPage));

	cache->Lock();

	off_t cacheOffset = oldPage->cache_offset << PAGE_SHIFT;

	// Notify not busy events for old page
	cache->NotifyPageEvents(oldPage, PAGE_EVENT_NOT_BUSY);

	// Remove old page from cache
	cache->RemovePage(oldPage);

	// Remove old page from page queue
	pageQueue.RemoveUnlocked(oldPage);

	// Discard bits of previous page
	oldPage->accessed = 0;
	oldPage->modified = 0;
	oldPage->usage_count = 0;

	// Replace page with new one
	cache->InsertPage(newPage, cacheOffset);

	// Set old page as free
	oldPage->SetState(PAGE_STATE_FREE);

	// Unmark new page from being busy
	cache->MarkPageUnbusy(newPage);

#if defined(DEBUG_MEMORY_HOTPLUG)
	printk(KERN_ERR | WITHUART, "Replace page %" B_PRIxPHYSADDR " with %" B_PRIxPHYSADDR "\n", page_to_phys(oldPage),
	       page_to_phys(newPage));
#endif

	return true;
}

int vm_detach_memory_etc(vm_paddr_t phys, size_t size, void *driver_tag, uint32_t ns_tag) {
#if defined(DEBUG_MEMORY_HOTPLUG)
	printk(KERN_ERR | WITHUART, "Try to detach zone, %d lomem pages free\n",
	       __atomic_load_n(&sMemoryZone[TASK_ZONE_INTERNAL].fUnreservedFreePages,
	                       __ATOMIC_RELAXED));
#endif

	// Allow only one instance of hotplug operation
	// at a time
	MutexLocker hotplugMutex(sMemoryHotplugMutex);
	WriteLocker segmentsLocker(sPageSegmentsRWLock);

	MemorySegment *seg = nullptr;
	int segmentIndex = -1;

	for (int i = 0; i < sPageSegmentCount; ++i) {
		if (sPageSegments[i] == nullptr)
			continue;

		if (sPageSegments[i]->base_phys != phys)
			continue;

		if (sPageSegments[i]->orig_size != size)
			continue;

		if (sPageSegments[i]->dynamic_tag != driver_tag)
			continue;

		if (sPageSegments[i]->nonsecure_tag != ns_tag)
			continue;

		if (sPageSegments[i]->zone_index != TASK_ZONE_EXTERNAL)
			continue;

		seg = sPageSegments[i];

		if (seg->disconnect_pending) {
			printk(KERN_ERR | WITHUART, "Segment disconnection already pednding\n");
			return -EBUSY;
		}

		seg->disconnect_pending = true;
		segmentIndex = i;
		break;
	}

	if (!seg) {
		printk(KERN_ERR | WITHUART, "Unable to find segment for disconnection\n");
		return -EINVAL;
	}

	// Drop the locks as we don't want to lock
	// rest of the system
	segmentsLocker.Unlock();
	hotplugMutex.Unlock();

	size_t pagesNeeded = seg->end_pfn - seg->base_pfn;
	size_t memoryNeeded = seg->end_phys - seg->base_phys;

#if defined(DEBUG_MEMORY_HOTPLUG)
	printk(KERN_ERR | WITHUART, "%zd pages needed, %zd bytes needed\n", pagesNeeded, memoryNeeded);
#endif

	// Build page list
	VMPageQueue::PageList acquiredFreePages, acquiredClearPages;
	size_t countAcquiredFreePages = 0, countAcquiredClearPages = 0;

	time_t start_time = system_time();
	time_t end_time = start_time + SECONDS(10);
	MemoryZone &zone = sMemoryZone[seg->zone_index];

	vm_reservation_t reservation;
	uint32_t numberOfReservedPagesForMigration = 0;

	// Preallocate small page pool
	if (vm_page_try_reserve_pages(&reservation, 32, GFP_PRIO_USER)) {
		numberOfReservedPagesForMigration = 32;
	}

	// Try to release memory in reasonable amount of time
	while ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded &&
	       system_time() < end_time) {
		bool needCPURelease = false;
		bool migrationFailed = false;
		uint32_t numberOfPagesNeededForMigration = 0;

#if defined(DEBUG_MEMORY_HOTPLUG)
		printk(KERN_ERR | WITHUART, "--> STEP 1. %zd pages acquired, %zd still needed\n",
		       (countAcquiredFreePages + countAcquiredClearPages),
		       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif

		auto queueRemover = [&](VMPageQueue &queue, [[maybe_unused]] uint8_t queueState) {
			uint32_t reserveToFree = 0;
			uint32_t maxCount = 1024;

			while (maxCount > 0 && (countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
				if (reserveToFree == 0) {
					uint32_t stillRemaining = pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages);

					uint32_t wantedToReserve = std::min<uint32_t>(std::min(stillRemaining, maxCount), 128);

					reserveToFree = reserve_some_pages_from_zone(
					        wantedToReserve, 0, zone);

					// Nothing left anymore
					if (reserveToFree == 0)
						break;
				}

				InterruptsSpinLocker freeQueueLocker(queue.GetLock());

				struct page *page = queue.Head();

				if (!page)
					break;

				--maxCount;

				if (page->segment != seg) {
					if (queue.Count() == 1)
						break;

					queue.Requeue(page, true);
					continue;
				}

				// Take one
				--reserveToFree;

				// Queue must be locked
				queue.Remove(page);

				// Mark page as busy so nobody will touch it
				page->busy = 1;

				freeQueueLocker.Unlock();

				if (page->State() == PAGE_STATE_CLEAR) {
					++countAcquiredClearPages;
					acquiredClearPages.Add(page);
				} else {
					ASSERT(page->State() == PAGE_STATE_FREE);
					++countAcquiredFreePages;
					acquiredFreePages.Add(page);
				}

#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "FREE Page %" B_PRIxPHYSADDR " consumed (%zd remaining)\n",
				       page_to_phys(page),
				       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif
			}

			if (reserveToFree > 0) {
				unreserve_pages_for_zone(reserveToFree, zone);
			}
		};

		// Step 1 - remove some free and cleared pages
		if ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
			WriteLocker allocatorLocker(zone.fFreePageQueuesLock);
			queueRemover(zone.fFreePageQueue, PAGE_STATE_FREE);
			queueRemover(zone.fClearPageQueue, PAGE_STATE_CLEAR);
		}

#if defined(DEBUG_MEMORY_HOTPLUG)
		printk(KERN_ERR | WITHUART, "--> STEP 2. %zd pages acquired, %zd still needed\n",
		       (countAcquiredFreePages + countAcquiredClearPages),
		       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif


		// Step 2 - free all cached pages regardless of memory segment
		if ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
			struct page marker = {};
			init_page_marker(marker, zone);

			// Just release all cached pages from this zone
			uint32_t pagesFreed = 0;
			uint32_t pagesGrabbed = 0;
			uint32_t pagesToFree = zone.fCachedPageQueue.Count();

			while ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded && (pagesGrabbed + pagesFreed) < pagesToFree) {
				struct page *page = find_cached_page_candidate(marker, zone);

				if (page == NULL)
					break;

				if (free_cached_page(page, true, zone, true)) {
					// Page must be marked busy so nobody is touching it
					ASSERT(page->busy);

					if (page->segment == seg) {
						// Page comes from our segment. Just grab it to our pool
						page->SetState(PAGE_STATE_FREE);

						acquiredFreePages.Add(page);
						++countAcquiredFreePages;
						++pagesGrabbed;

#if defined(DEBUG_MEMORY_HOTPLUG)
						printk(KERN_ERR | WITHUART, "CACHED page %" B_PRIxPHYSADDR "migrated, %zd remaining\n",
						       page_to_phys(page),
						       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif
					} else {
						// Page might come from another segment. Put it back to the free queue
						WriteLocker locker(zone.fFreePageQueuesLock);
						page->busy = 0;// Unmark page busy
						page->SetState(PAGE_STATE_FREE);
						zone.fFreePageQueue.PrependUnlocked(page);
						locker.Unlock();
						pagesFreed++;

#if defined(DEBUG_MEMORY_HOTPLUG)
						printk(KERN_ERR | WITHUART,
						       "CACHED page %" B_PRIxPHYSADDR " freed (not our region)\n",
						       page_to_phys(page));
#endif
					}
				}
			}

			remove_page_marker(marker, zone);

			if (pagesFreed > 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "We've freed %u extra pages in this run\n", pagesFreed);
#endif

				// We might have freed some pages
				unreserve_pages_for_zone(pagesFreed, zone);
			}
		}

#if defined(DEBUG_MEMORY_HOTPLUG)
		printk(KERN_ERR | WITHUART, "--> STEP 3. %zd pages acquired, %zd still needed\n",
		       (countAcquiredFreePages + countAcquiredClearPages),
		       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif

		// Step 3 - go through list of active pages
		// and try to write back or deactivate them
		if ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
			struct page marker = {};
			init_page_marker(marker, zone);

			VMPageQueue &queue = sActivePageQueue;
			uint32_t modifiedPageCount = 0;

			while ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
				InterruptsSpinLocker queueLocker(queue.GetLock());

				// get the next page
				struct page *page;

				if (marker.State() == PAGE_STATE_UNUSED) {
					page = queue.Head();
				} else {
					page = queue.Next(&marker);
					queue.Remove(&marker);
					marker.SetState(PAGE_STATE_UNUSED);
				}

				if (page == NULL)
					break;

				marker.SetState(PAGE_STATE_ACTIVE);
				queue.InsertAfter(page, &marker);

				// Drop the lock as we don't need it anymore
				queueLocker.Unlock();

				// This is not our page
				if (page->segment != seg || page->busy)
					continue;

				VMCache *cache = vm_cache_acquire_locked_page_cache(page, false);

				if (!cache) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "ACTIVE page %" B_PRIxPHYSADDR " - can't acquire page cache\n",
					       page_to_phys(page));
#endif

					continue;
				}

				AutoLocker<VMCache> cacheLocker(cache, true);
				MethodDeleter<VMCache> _2(cache, &VMCache::ReleaseRefLocked);

				if (page->busy) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "ACTIVE page %" B_PRIxPHYSADDR " is busy\n",
					       page_to_phys(page));
#endif

					continue;
				}

				// Skip busy pages as we don't want to touch them
				if (page->State() != PAGE_STATE_ACTIVE) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "ACTIVE page %" B_PRIxPHYSADDR " changed state to %d\n",
					       page_to_phys(page), page->State());
#endif

					continue;
				}

				// Skip wired pages as we don't want to touch them
				if (page->WiredCount() > 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "ACTIVE page %" B_PRIxPHYSADDR " is wired\n",
					       page_to_phys(page));
#endif

					continue;
				}

				// Remove all mappings of page and mark page as busy
				// so other subsystems won't be able to touch it
				vm_remove_all_page_mappings(page);

				ASSERT(!page->IsMapped());

				page->usage_count = 0;

				if (page->modified) {
					// Try migration of page to the internal zone.
					// If this fails, we will issue write to the backing
					// store of the cache
					if (!try_migrate_page_to_internal_zone(cache, page, queue,
					                                       numberOfReservedPagesForMigration, reservation)) {
#if defined(DEBUG_MEMORY_HOTPLUG)
						printk(KERN_ERR | WITHUART,
						       "ACTIVE+modified page %" B_PRIxPHYSADDR ". Need writeback\n",
						       page_to_phys(page));
#endif

						// Change the page's state to modified. We will wake up
						// page daemon later to issue page write
						vm_page_set_state(page, PAGE_STATE_MODIFIED);
						++modifiedPageCount;
					} else {
						ASSERT(page->State() == PAGE_STATE_FREE);
						ASSERT(page->busy);

						// Append to the local queue
						acquiredFreePages.Add(page);
						++countAcquiredFreePages;
					}
				} else {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "Make page %" B_PRIxPHYSADDR " INACTIVE\n",
					       page_to_phys(page));
#endif
					// Move page to the INACTIVE queue
					vm_page_set_state(page, PAGE_STATE_INACTIVE);
				}
			}

			// Remove marker page
			remove_page_marker(marker, zone);

			// Issue page writer if needed
			if (modifiedPageCount > 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "Wake up page daemon because of %u dirty ACTIVE pages\n",
				       modifiedPageCount);
#endif

				sPageWriterCondition.WakeUp();
				needCPURelease = true;
			}
		}

#if defined(DEBUG_MEMORY_HOTPLUG)
		printk(KERN_ERR | WITHUART, "--> STEP 4. %zd pages acquired, %zd still needed\n",
		       (countAcquiredFreePages + countAcquiredClearPages),
		       pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));
#endif

		// Step 4 - Go through the list of inactive pages
		// and try to free them if possible. If page would require
		// migration, we count number of such pages and try
		// reserve them later so system will not get OOM suddenly
		// after mass page migration
		if ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
			struct page marker = {};
			init_page_marker(marker, zone);

			VMPageQueue &queue = sInactivePageQueue;
			uint32_t modifiedPageCount = 0;

			while ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded) {
				InterruptsSpinLocker queueLocker(queue.GetLock());

				// get the next page
				struct page *page;

				if (marker.State() == PAGE_STATE_UNUSED) {
					page = queue.Head();
				} else {
					page = queue.Next(&marker);
					queue.Remove(&marker);
					marker.SetState(PAGE_STATE_UNUSED);
				}

				if (page == NULL)
					break;

				marker.SetState(PAGE_STATE_INACTIVE);
				queue.InsertAfter(page, &marker);

				// Drop the lock as we don't need it anymore
				queueLocker.Unlock();

				// This is not our page
				if (page->segment != seg || page->busy)
					continue;

				VMCache *cache = vm_cache_acquire_locked_page_cache(page, false);

				if (!cache) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "INACTIVE page %" B_PRIxPHYSADDR " - can't acquire page cache\n",
					       page_to_phys(page));
#endif

					continue;
				}

				AutoLocker<VMCache> cacheLocker(cache, true);
				MethodDeleter<VMCache> _2(cache, &VMCache::ReleaseRefLocked);

				// Skip busy pages as we don't want to touch them
				if (page->busy) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "INACTIVE page %" B_PRIxPHYSADDR " is busy\n",
					       page_to_phys(page));
#endif

					continue;
				}


				// Skip wired pages as we don't want to touch them
				if (page->State() != PAGE_STATE_INACTIVE) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "INACTIVE page %" B_PRIxPHYSADDR " changed state to %d\n",
					       page_to_phys(page), page->State());
#endif

					continue;
				}

				// Skip wired pages as we don't want to touch them
				if (page->WiredCount() > 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
					printk(KERN_ERR | WITHUART, "INACTIVE page %" B_PRIxPHYSADDR " is wired\n",
					       page_to_phys(page));
#endif

					continue;
				}

				vm_remove_all_page_mappings(page);

				ASSERT(!page->IsMapped());

				page->usage_count = 0;

				if (!page->modified) {
					if (!cache->HasPage(page->cache_offset * PAGE_SIZE)) {
						// We must migrate page to the internal zone if backing
						// store doesn't have it
						if (!try_migrate_page_to_internal_zone(cache, page, queue,
						                                       numberOfReservedPagesForMigration, reservation)) {
#if defined(DEBUG_MEMORY_HOTPLUG)
							printk(KERN_ERR | WITHUART, "INACTIVE+clean page %" B_PRIxPHYSADDR ". Needs migration\n", page_to_phys(page));
#endif
							// This page needs migration
							++numberOfPagesNeededForMigration;
						} else {
							ASSERT(page->State() == PAGE_STATE_FREE);
							ASSERT(page->busy);

							// Append to the local queue
							acquiredFreePages.Add(page);
							++countAcquiredFreePages;
						}
					} else {
#if defined(DEBUG_MEMORY_HOTPLUG)
						printk(KERN_ERR | WITHUART, "INACTIVE+freeable page %" B_PRIxPHYSADDR ". %zd remaining\n",
						       page_to_phys(page),
						       pagesNeeded - ((countAcquiredFreePages + countAcquiredClearPages) + 1));
#endif

						// Remove page from the inactive queue
						set_page_state(page, PAGE_STATE_UNUSED);
						// Page is clean so we can remove it from cache
						cache->RemovePage(page);
						// We can safely release the cache
						// Mark page as free so nobody else can touch it
						page->SetState(PAGE_STATE_FREE);
						// Mark page as busy so it's skipped by page daemon
						page->busy = 1;

						// Add page to our queue
						acquiredFreePages.Add(page);
						++countAcquiredFreePages;
					}
				} else {
					// Try migration of page to the internal zone.
					// If this fails, we will issue write to the backing
					// store of the cache
					if (!try_migrate_page_to_internal_zone(cache, page, queue,
					                                       numberOfReservedPagesForMigration, reservation)) {
#if defined(DEBUG_MEMORY_HOTPLUG)
						printk(KERN_ERR | WITHUART, "INACTIVE+dirty page %" B_PRIxPHYSADDR ". Needs migration\n", page_to_phys(page));
#endif

						// This page needs migration. We don't want to write back
						// the data as it may fail and it would hog the external
						// memory segment indefinitely.
						++numberOfPagesNeededForMigration;
					} else {
						ASSERT(page->State() == PAGE_STATE_FREE);
						ASSERT(page->busy);

						// Append to the local queue
						acquiredFreePages.Add(page);
						++countAcquiredFreePages;
					}
				}
			}

			remove_page_marker(marker, zone);

			// Issue page writer if needed
			if (modifiedPageCount > 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "Wake up page daemon because of %u dirty INACTIVE pages\n", modifiedPageCount);
#endif

				sPageWriterCondition.WakeUp();
				needCPURelease = true;
			}
		}

		if ((countAcquiredFreePages + countAcquiredClearPages) < pagesNeeded && numberOfPagesNeededForMigration > 0) {
			// Cap to the number of needed pages
			numberOfPagesNeededForMigration = std::min<uint32_t>(
			        numberOfPagesNeededForMigration,
			        pagesNeeded - (countAcquiredFreePages + countAcquiredClearPages));

#if defined(DEBUG_MEMORY_HOTPLUG)
			printk(KERN_ERR | WITHUART,
			       "%" PRIu32 " pages are needed for page migration\n",
			       numberOfPagesNeededForMigration);
#endif

			if (numberOfReservedPagesForMigration > 0) {
				vm_page_unreserve_pages(&reservation);
				numberOfReservedPagesForMigration = 0;
			}

			// Try to reserve pages from the internal zone
			if (vm_page_try_reserve_pages(&reservation,
			                              numberOfPagesNeededForMigration, GFP_PRIO_USER)) {
				numberOfReservedPagesForMigration = numberOfPagesNeededForMigration;

#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "Acquired %" PRIu32 " pages for migration\n",
				       numberOfReservedPagesForMigration);
#endif
			} else {
#if defined(DEBUG_MEMORY_HOTPLUG)
				printk(KERN_ERR | WITHUART, "Can't reserve pages for memory migration\n");
#endif

				migrationFailed = true;
			}
		}

		if (migrationFailed) {
			// There's no point to continue
			break;
		}

		// Give some time to page writer and other tasks so they can recover
		if (needCPURelease) {
			snooze(MILLISECS(800));
		} else {
			snooze(MILLISECS(50));
		}
	}

	if (numberOfReservedPagesForMigration > 0) {
		vm_page_unreserve_pages(&reservation);
		numberOfReservedPagesForMigration = 0;
	}

	// 1. We need to reserve all pages
	//
	// 2. We need to grab required amount of memory so system
	//    is able to release pages
	if ((countAcquiredFreePages + countAcquiredClearPages) != pagesNeeded || vm_try_reserve_memory(memoryNeeded, GFP_PRIO_USER, SECONDS(5)) < 0) {
#if defined(DEBUG_MEMORY_HOTPLUG)
		printk(KERN_ERR | WITHUART,
		       "Failed to release all pages in timely manner\n");
#endif

		// Unbusy all pages from queue
		for (auto page = acquiredFreePages.Head(); page; page =
		                                                         acquiredFreePages.GetNext(page)) {
			ASSERT(page->busy);
			ASSERT(page->State() == PAGE_STATE_FREE);
			page->busy = 0;
		}

		for (auto page = acquiredClearPages.Head(); page; page =
		                                                          acquiredClearPages.GetNext(page)) {
			ASSERT(page->busy);
			ASSERT(page->State() == PAGE_STATE_CLEAR);
			page->busy = 0;
		}

		WriteLocker queueLocker(zone.fFreePageQueuesLock);

		zone.fFreePageQueue.AppendUnlocked(acquiredFreePages,
		                                   countAcquiredFreePages);
		zone.fClearPageQueue.AppendUnlocked(acquiredClearPages,
		                                    countAcquiredClearPages);

		queueLocker.Unlock();

		// Give pages back to the system
		unreserve_pages_for_zone((countAcquiredFreePages + countAcquiredClearPages), zone);

		// Unmark segment disconnection as pending
		hotplugMutex.Lock();
		segmentsLocker.Lock();
		seg->disconnect_pending = false;
		return -ETIMEDOUT;
	}

	// Sanity check to make sure all pages are busy
	for (size_t i = 0; i < pagesNeeded; ++i) {
		KASSERT_ALWAYS(seg->pages[i].busy, ("External zone page not busy"));
		KASSERT_ALWAYS(seg->pages[i].cache_ref == nullptr,
		               ("External page still has cache"));
		KASSERT_ALWAYS(seg->pages[i].State() == PAGE_STATE_CLEAR ||
		                       seg->pages[i].State() == PAGE_STATE_FREE,
		               ("Page is in state %" PRIu8, seg->pages[i].State()));
	}

	hotplugMutex.Lock();

	// Remove segment from memory segment array
	segmentsLocker.Lock();

	{
		// Make sure memory is disconnected
		InterruptsSmpWriteSpinLocker segmentSpinLocker(sPageSegmentsRWSpinLock);
		sPageSegments[segmentIndex] = nullptr;
	}

	segmentsLocker.Unlock();

	// Unreserve memory to allow tasks to use it
	{
		MutexLocker locker(sAvailableMemoryLock);

		// Remove these pages from the pool
		zone.fTotalPages -= pagesNeeded;

		// Subtract memory limit
		sTotalMemory -= memoryNeeded;

		// Recalculate memory limits for page daemon
		recalculate_free_targets(zone);

		if (zone.fTotalPages == 0) {
			locker.Unlock();

			MutexLocker unavailableLocker(zone.fPageDeficitLock);

			// Reject all threads waiting for memory
			// as we can't satisfy reservations anymore
			auto it = zone.fPageReservationWaiters.GetIterator();
			while (it.HasNext()) {
				it.Next()->cond.NotifyAll(-EAGAIN);
			}
		}
	}

	hotplugMutex.Unlock();

	memset(seg->pages, 0xcc, sizeof(struct page) * pagesNeeded);
	// Remove underlying pages
	delete_area(seg->pages_area_id);

	delete seg;

	// We're done
	return 0;
}
#endif

extern "C" void vm_cache_init(void);

static void reserve_boot_ranges() {
	for (uint32_t i = 0; i < gKernelArgs->virt_memory.allocated.num; ++i) {
		uint64_t rangeBase = gKernelArgs->virt_memory.allocated.ranges[i].start;
		uint64_t rangeEnd = rangeBase + gKernelArgs->virt_memory.allocated.ranges[i].size;

		rangeBase = std::max<uint64_t>(rangeBase,
		                               VMAddressSpace::Kernel()->Base());
		rangeEnd = std::min<uint64_t>(rangeEnd,
		                              VMAddressSpace::Kernel()->EndAddress());

		if (rangeBase < rangeEnd) {
			vaddr_t address = rangeBase;

			int status = vm_reserve_address_range(VMAddressSpace::KernelID(),
			                                      &address, B_EXACT_ADDRESS, rangeEnd - rangeBase, 0, 0);

			if (status < 0)
				panic("Can't reserve initial ranges");
		}
	}
}

static void reserve_boot_pages() {
	for (uint32_t i = 0; i < gKernelArgs->phy_memory.allocated.num; ++i) {
		mark_page_range_in_use(gKernelArgs->phy_memory.allocated.ranges[i].start >> PAGE_SHIFT,
		                       gKernelArgs->phy_memory.allocated.ranges[i].size >> PAGE_SHIFT,
		                       true);
	}
}

extern "C" {
extern char _stext[];
extern char _etext[];
extern char _sdata[];
extern char _end[];
}

extern "C" void kernel_weak_ref_cache_init(void);

static void
allocate_kernel_args() {
	for (uint32_t i = 0; i < gKernelArgs->kargs_memory.num; i++) {
		void *address = (void *) (uintptr_t) gKernelArgs->kargs_memory.ranges[i].start;

		if (create_area("_kernel args_", &address, B_EXACT_ADDRESS,
		                gKernelArgs->kargs_memory.ranges[i].size, B_ALREADY_WIRED,
		                B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) < 0) {
			panic("Can't convert kernel args area");
		}
	}

	for (uint32_t i = 0; i < gKernelArgs->loader_memory.num; i++) {
		void *address = (void *) (uintptr_t) gKernelArgs->loader_memory.ranges[i].start;

		if (create_area("kldr ram", &address, B_EXACT_ADDRESS,
		                gKernelArgs->loader_memory.ranges[i].size, B_ALREADY_WIRED,
		                B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) < 0) {
			panic("Can't convert kldr area");
		}
	}

	void *address = gKernelArgs;
	if (create_area("boot loader args", &address, B_EXACT_ADDRESS,
	                roundup2(gKernelArgs->total_size, PAGE_SIZE),
	                B_ALREADY_WIRED,
	                B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) < 0) {
		panic("Can't convert kernel args area");
	}
}

static void
create_preloaded_image_areas(struct preloaded_image *image) {
	char name[B_OS_NAME_LENGTH];
	int32_t length;

	// use file name to create a good area name
	char *fileName = strrchr(image->name, '/');
	if (fileName == nullptr)
		fileName = image->name;
	else
		fileName++;

	length = strlen(fileName);
	// make sure there is enough space for the suffix
	if (length > 25)
		length = 25;

	for (uint8_t i = 0; i < image->num_regions; ++i) {
		const char *tag;

		uint32_t protection = B_KERNEL_READ_AREA;

		if (image->regions[i].protection & PF_W)
			protection |= B_KERNEL_WRITE_AREA;

		if (image->regions[i].protection & PF_X)
			protection |= B_KERNEL_EXECUTE_AREA;

		image->regions[i].protection = protection;

		if (image->regions[i].protection & B_KERNEL_EXECUTE_AREA)
			tag = "text";
		else if (image->regions[i].protection & B_KERNEL_WRITE_AREA)
			tag = "data";
		else
			tag = "ro";

		snprintf(name, sizeof(name), "%s_%s", fileName, tag);
		void *base = (void *) image->regions[i].start;

		image->regions[i].id = create_area(name, &base, B_EXACT_ADDRESS,
		                                   image->regions[i].size, B_ALREADY_WIRED,
		                                   image->is_module ? B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA : image->regions[i].protection);

		if (image->regions[i].id < 0) {
			panic("Can't create image area");
		}
	}
}

#if defined(CONFIG_KDEBUGGER)
const char *page_state_to_string(int state) {
	switch (state) {
		case PAGE_STATE_ACTIVE:
			return "active";
		case PAGE_STATE_INACTIVE:
			return "inactive";
		case PAGE_STATE_MODIFIED:
			return "modified";
		case PAGE_STATE_CACHED:
			return "cached";
		case PAGE_STATE_FREE:
			return "free";
		case PAGE_STATE_CLEAR:
			return "clear";
		case PAGE_STATE_WIRED:
			return "wired";
		case PAGE_STATE_UNUSED:
			return "unused";
		default:
			return "unknown";
	}
}

static int dump_page(int argc, char **argv) {
	bool addressIsPointer = true;
	bool physical = false;
	bool searchMappings = false;
	int32_t index = 1;

	while (index < argc) {
		if (argv[index][0] != '-')
			break;

		if (!strcmp(argv[index], "-p")) {
			addressIsPointer = false;
			physical = true;
		} else if (!strcmp(argv[index], "-v")) {
			addressIsPointer = false;
		} else if (!strcmp(argv[index], "-m")) {
			searchMappings = true;
		} else {
			print_debugger_command_usage(argv[0]);
			return 0;
		}

		index++;
	}

	if (index + 1 != argc) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	uint64_t value;
	if (!evaluate_debug_expression(argv[index], &value, false))
		return 0;

	uint64_t pageAddress = value;
	struct page *page;

	if (addressIsPointer) {
		page = (struct page *) (uintptr_t) pageAddress;
	} else {
		if (!physical) {
			VMAddressSpace *addressSpace = VMAddressSpace::Kernel();

			if (debug_get_debugged_thread()->task->addressSpace != nullptr)
				addressSpace = debug_get_debugged_thread()->task->addressSpace;

			uint32_t flags = 0;
			vm_paddr_t physicalAddress;
			if (addressSpace->TranslationMap()->QueryInterrupt(pageAddress,
			                                                   &physicalAddress, &flags) != 0 ||
			    (flags & PAGE_PRESENT) == 0) {
				kprintf("Virtual address not mapped to a physical page in this "
				        "address space.\n");
				return 0;
			}
			pageAddress = physicalAddress;
		}

		page = pfn_to_page_locked(pageAddress / PAGE_SIZE);
	}

	if(!page) {
		kprintf("NULL page\n");
		return 0;
	}

	kprintf("PAGE: %p\n", page);
	kprintf("queue_next,prev: %p, %p\n", page->queue_link.next,
	        page->queue_link.previous);
	kprintf("physical_number: %#" B_PRIxPHYSADDR "\n",
	        (vm_paddr_t) page->pfn);
	kprintf("cache:           %p\n", page->Cache());
	kprintf("cache_offset:    %zu\n", (size_t) page->cache_offset);
	kprintf("cache_next:      %p\n", page->cache_next);
	kprintf("state:           %s\n", page_state_to_string(page->State()));
	kprintf("wired_count:     %d\n", page->WiredCount());
	kprintf("usage_count:     %d\n", page->usage_count);
	kprintf("busy:            %d\n", page->busy);
	kprintf("busy_writing:    %d\n", page->busy_writing);
	kprintf("accessed:        %d\n", page->accessed);
	kprintf("modified:        %d\n", page->modified);
#if defined(CONFIG_DEBUG)
	kprintf("queue:           %p\n", page->queue);
#endif
	//#if DEBUG_PAGE_ACCESS
	//		kprintf("accessor:        %" PRId32 "\n", page->accessing_thread);
	//	#endif
	kprintf("area mappings:\n");

	vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
	vm_page_mapping *mapping;
	while ((mapping = iterator.Next()) != nullptr) {
		kprintf("  %p (%" PRId32 ")\n", mapping->area, mapping->area->id);
		mapping = mapping->page_link.next;
	}

	if (searchMappings) {
		kprintf("all mappings:\n");
		VMAddressSpace *addressSpace = VMAddressSpace::DebugFirst();
		while (addressSpace != nullptr) {
			for (uintptr_t address = addressSpace->Base(); address != addressSpace->EndAddress();
			     address += PAGE_SIZE) {
				vm_paddr_t physicalAddress;
				uint32_t flags = 0;
				if (addressSpace->TranslationMap()->QueryInterrupt(address, &physicalAddress, &flags) == 0 && (flags & PAGE_PRESENT) != 0 && physicalAddress / PAGE_SIZE == page->pfn) {
					VMArea *area = addressSpace->LookupArea(address);
					kprintf("  aspace %" PRId32 ", area %" PRId32 ": %#" PRIxPTR " (%c%c%s%s)\n", addressSpace->ID(),
					        area != nullptr ? area->id : -1, address,
					        (flags & B_KERNEL_READ_AREA) != 0 ? 'r' : '-',
					        (flags & B_KERNEL_WRITE_AREA) != 0 ? 'w' : '-',
					        (flags & PAGE_MODIFIED) != 0 ? " modified" : "",
					        (flags & PAGE_ACCESSED) != 0 ? " accessed" : "");
				}
			}
			addressSpace = VMAddressSpace::DebugNext(addressSpace);
		}
	}

	set_debug_variable("_cache", (uintptr_t) page->Cache());
	//#if DEBUG_PAGE_ACCESS
	//		set_debug_variable("_accessor", page->accessing_thread);
	//#endif

	return 0;
}

const char *vm_cache_type_to_string(int32_t type);

static void do_dump_queue(int argc, VMPageQueue *queue) {
	kprintf("queue = %p, queue->head = %p, queue->tail = %p, queue->count = %zu\n", queue, queue->Head(), queue->Tail(),
	        queue->Count());

	if (argc == 3) {
		struct page *page = queue->Head();

		kprintf("page        cache       type       state  wired  usage\n");
		for (vm_pindex_t i = 0; page; i++, page = queue->Next(page)) {
			kprintf("%p  %p  %-7s %8s  %5d  %5d\n", page, page->Cache(),
			        page->Cache() ? vm_cache_type_to_string(page->Cache()->type) : "<NoCache>",
			        page_state_to_string(page->State()),
			        page->WiredCount(), page->usage_count);
		}
	}
}

static int
dump_page_queue(int argc, char **argv) {
	if (argc < 2) {
		kprintf("usage: page_queue <address/name> [list]\n");
		return 0;
	}

	if (strlen(argv[1]) >= 2 && argv[1][0] == '0' && argv[1][1] == 'x') {
		unsigned long v = strtoul(argv[1], nullptr, 16);
		if (v == 0 || v == ULONG_MAX) {
			kprintf("strtoul error for '%s'\n", argv[1]);
			return 0;
		}
		do_dump_queue(argc, (VMPageQueue *) v);
	} else if (!strcmp(argv[1], "free")) {
		for (auto &i : sMemoryZone) {
			do_dump_queue(argc, &i.fFreePageQueue);
		}
	} else if (!strcmp(argv[1], "clear")) {
		for (auto &i : sMemoryZone) {
			do_dump_queue(argc, &i.fClearPageQueue);
		}
	} else if (!strcmp(argv[1], "modified"))
		do_dump_queue(argc, &sModifiedPageQueue);
	else if (!strcmp(argv[1], "active"))
		do_dump_queue(argc, &sActivePageQueue);
	else if (!strcmp(argv[1], "inactive"))
		do_dump_queue(argc, &sInactivePageQueue);
	else if (!strcmp(argv[1], "cached")) {
		for (auto &i : sMemoryZone) {
			do_dump_queue(argc, &i.fCachedPageQueue);
		}
	} else {
		kprintf("page_queue: unknown queue \"%s\".\n", argv[1]);
		return 0;
	}


	return 0;
}

static int
dump_page_stats(int, char **) {
	size_t swappableModified = 0;
	size_t swappableModifiedInactive = 0;

	size_t counter[8];
	size_t busyCounter[8];
	memset(counter, 0, sizeof(counter));
	memset(busyCounter, 0, sizeof(busyCounter));

	struct page_run {
		vm_pindex_t start;
		vm_pindex_t end;

		[[nodiscard]] vm_pindex_t Length() const { return end - start; }
	};

	for (int index = 0; index < sPageSegmentCount; ++index) {
		if (!sPageSegments[index])
			continue;

		for (vm_pindex_t i = 0; i < (sPageSegments[index]->end_pfn - sPageSegments[index]->base_pfn); i++) {
			if (sPageSegments[index]->pages[i].State() > 7) {
				panic("page %" B_PRIuPHYSADDR " at %p has invalid state!\n", i,
				      &sPageSegments[index]->pages[i]);
			}

			uint32_t pageState = sPageSegments[index]->pages[i].State();

			counter[pageState]++;
			if (sPageSegments[index]->pages[i].busy)
				busyCounter[pageState]++;

			if (pageState == PAGE_STATE_MODIFIED && sPageSegments[index]->pages[i].Cache() != nullptr && sPageSegments[index]->pages[i].Cache()->temporary && sPageSegments[index]->pages[i].WiredCount() == 0) {
				swappableModified++;
				if (sPageSegments[index]->pages[i].usage_count == 0)
					swappableModifiedInactive++;
			}
		}
	}

	kprintf("page stats:\n");
	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		kprintf("zone:%d total: %d\n", i, sMemoryZone[i].fTotalPages);
		kprintf("zone:%d cached: %"
		        "zu"
		        " (busy: %"
		        "zu"
		        ")\n",
		        i, counter[PAGE_STATE_CACHED], busyCounter[PAGE_STATE_CACHED]);
	}

	kprintf("active: %"
	        "zu"
	        " (busy: %"
	        "zu"
	        ")\n",
	        counter[PAGE_STATE_ACTIVE], busyCounter[PAGE_STATE_ACTIVE]);
	kprintf("inactive: %"
	        "zu"
	        " (busy: %"
	        "zu"
	        ")\n",
	        counter[PAGE_STATE_INACTIVE], busyCounter[PAGE_STATE_INACTIVE]);
	kprintf("unused: %"
	        "zu"
	        " (busy: %"
	        "zu"
	        ")\n",
	        counter[PAGE_STATE_UNUSED], busyCounter[PAGE_STATE_UNUSED]);
	kprintf("wired: %"
	        "zu"
	        " (busy: %"
	        "zu"
	        ")\n",
	        counter[PAGE_STATE_WIRED], busyCounter[PAGE_STATE_WIRED]);
	kprintf("modified: %"
	        "zu"
	        " (busy: %"
	        "zu"
	        ")\n",
	        counter[PAGE_STATE_MODIFIED], busyCounter[PAGE_STATE_MODIFIED]);
	kprintf("free: %"
	        "zu"
	        "\n",
	        counter[PAGE_STATE_FREE]);
	kprintf("clear: %"
	        "zu"
	        "\n",
	        counter[PAGE_STATE_CLEAR]);

	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		kprintf("zone:%d unreserved free pages: %" PRId32 "\n",
		        i, __atomic_load_n(&sMemoryZone[i].fUnreservedFreePages, __ATOMIC_RELAXED));
		kprintf("zone:%d unsatisfied page reservations: %" PRId32 "\n",
		        i, __atomic_load_n(&sMemoryZone[i].fUnsatisfiedPageReservations, __ATOMIC_RELAXED));
	}

	kprintf("mapped pages: %" PRId32 "\n",
	        __atomic_load_n(&gMappedPagesCount, __ATOMIC_RELAXED));

	kprintf("waiting threads:\n");
	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		for (PageReservationWaiterList::Iterator it = sMemoryZone[i].fPageReservationWaiters.GetIterator();
		     PageReservationWaiter *waiter = it.Next();) {
			kprintf("  %6" PRId32 ": missing: %6" PRIu32
			        ", don't touch: %6" PRIu32 ", zone: %d\n",
			        waiter->id,
			        waiter->missing, waiter->dontTouch, i);
		}
	}

	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		kprintf("\nzone:%d free queue: %p, count = %zu\n", i,
		        &sMemoryZone[i].fFreePageQueue, sMemoryZone[i].fFreePageQueue.Count());
		kprintf("clear queue: %p, count = %zu\n", &sMemoryZone[i].fClearPageQueue,
		        sMemoryZone[i].fClearPageQueue.Count());
	}

	kprintf("modified queue: %p, count = %zu (%d"
	        " temporary, %zu swappable, "
	        "inactive: %zu"
	        ")\n",
	        &sModifiedPageQueue, sModifiedPageQueue.Count(),
	        __atomic_load_n(&sModifiedTemporaryPages, __ATOMIC_RELAXED),
	        swappableModified, swappableModifiedInactive);
	kprintf("active queue: %p, count = %zu\n",
	        &sActivePageQueue, sActivePageQueue.Count());
	kprintf("inactive queue: %p, count = %zu\n",
	        &sInactivePageQueue, sInactivePageQueue.Count());
	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		kprintf("zone:%d cached queue: %p, count = %zu\n",
		        i, &sMemoryZone[i].fCachedPageQueue, sMemoryZone[i].fCachedPageQueue.Count());
	}
	return 0;
}

static int
find_page(int argc, char **argv) {
	struct page *page;
	uintptr_t address;
	int32_t index = 1;
	int i;

	struct {
		const char *name;
		VMPageQueue *queue;
	} pageQueueInfos[] = {
		//		{ "free",		&sFreePageQueue },
		//		{ "clear",		&sClearPageQueue },
		{ "modified", &sModifiedPageQueue },
		{ "active", &sActivePageQueue },
		{ "inactive", &sInactivePageQueue },
		//		{ "cached",		sCachedPageQueue },
		{ nullptr, nullptr }
	};

	if (argc < 2 || strlen(argv[index]) <= 2 || argv[index][0] != '0' || argv[index][1] != 'x') {
		kprintf("usage: find_page <address>\n");
		return 0;
	}

	address = strtoul(argv[index], nullptr, 0);
	page = (struct page *) address;

	for (i = 0; pageQueueInfos[i].name; i++) {
		VMPageQueue::Iterator it = pageQueueInfos[i].queue->GetIterator();
		while (struct page *p = it.Next()) {
			if (p == page) {
				kprintf("found page %p in queue %p (%s)\n", page,
				        pageQueueInfos[i].queue, pageQueueInfos[i].name);
				return 0;
			}
		}
	}

	kprintf("page %p isn't in any queue\n", page);

	return 0;
}
#endif

/*!
 * Bootstrap enough code so early page mapper can work
*/
void __init_text vm_init_mapper_early(struct kernel_args *args) {
	arch_vm_init_mapper_early(args);
	arch_vm_translation_map_init(&sPhysicalPageMapper);
}

void __init_text vm_init(void) {
	memory_manager_init();
	block_allocator_init_boot();
	vm_init_mappings();
	if (VMAreaHash::Init() < 0)
		panic("Can't create VMAreaHash");
	dpcpu_startup();

	sPageSegmentCount = gKernelArgs->all_memory.num;

	for (int i = 0; i < sPageSegmentCount; ++i) {
		sPageSegments[i] = new (std::nothrow) MemorySegment();
		if (sPageSegments[i] == nullptr)
			panic("Can't allocate memory segment");
	}

	for (int i = 0; i < sPageSegmentCount; ++i) {
		size_t pages_size;
		MemorySegment *seg = sPageSegments[i];

		pages_size = sizeof(struct page) * (gKernelArgs->all_memory.ranges[i].size >> PAGE_SHIFT);
		pages_size = ROUND_PGUP(pages_size);

		seg->base_pfn = gKernelArgs->all_memory.ranges[i].start >> PAGE_SHIFT;
		seg->end_pfn = (gKernelArgs->all_memory.ranges[i].start + gKernelArgs->all_memory.ranges[i].size) >> PAGE_SHIFT;
		seg->base_phys = gKernelArgs->all_memory.ranges[i].start;
		seg->end_phys = (gKernelArgs->all_memory.ranges[i].start + gKernelArgs->all_memory.ranges[i].size);
		seg->orig_size = gKernelArgs->all_memory.ranges[i].size;

		seg->pages = (struct page *) vm_allocate_early(pages_size, ~0L,
		                                               B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, PAGE_SIZE);

		memset(seg->pages, 0, pages_size);

		size_t num_pages = gKernelArgs->all_memory.ranges[i].size >> PAGE_SHIFT;

		for (size_t j = 0; j < num_pages; ++j) {
			auto *page = new (&seg->pages[j]) struct page();
#if defined(CONFIG_MEMORY_HOTPLUG)
			page->segment = seg;
#endif
			page->Init(j + seg->base_pfn);
			sMemoryZone[0].fFreePageQueue.Append(page);
		}

		sMemoryZone[0].fTotalPages += num_pages;
	}

	int error = VMAddressSpace::Init();

	if (error < 0)
		panic("Translation map creation failure");

	unreserve_pages_for_zone(sMemoryZone[0].fTotalPages, sMemoryZone[0]);
	vm_unreserve_memory((size_t) sMemoryZone[0].fTotalPages << PAGE_SHIFT);
	sTotalMemory += (size_t) sMemoryZone[0].fTotalPages << PAGE_SHIFT;

	recalculate_free_targets(sMemoryZone[0]);

	vm_cache_init();

	// Prevent creation of vm areas in kernel boot ranges
	reserve_boot_ranges();
	reserve_boot_pages();

	low_resource_manager_init();

	vm_page_allocator_active = true;
	memory_manager_init_post_area();
	arch_vm_translation_map_init_post_area();
	arch_vm_init_post_area(gKernelArgs);
	allocate_kernel_args();

	recalculate_free_targets(sMemoryZone[0]);

	create_preloaded_image_areas(gKernelArgs->kernel_image);

	// allocate areas for preloaded images
	for (auto *image = gKernelArgs->preloaded_images; image != nullptr; image = image->next)
		create_preloaded_image_areas(image);

	// allocate kernel stacks
	for (int i = 0; i < gKernelArgs->cpu_count; i++) {
		char name[64];

		snprintf(name, sizeof(name), "idle thread %d kstack", i + 1);
		void *address = (void *) (uintptr_t) gKernelArgs->cpu_kstack[i].start;

		area_id area = create_area(name, &address, B_EXACT_ADDRESS, gKernelArgs->cpu_kstack[i].size,
		                           B_ALREADY_WIRED, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

		if (area < 0) {
			panic("Can't convert kernel stack area");
		}
	}

	thread_preboot_early_convert_areas();

	// convert areas for rootfs
	if (gKernelArgs->rootfs_image.start) {
		void *rfs_base = (void *) (uintptr_t) gKernelArgs->rootfs_image.start;
		area_id rfs_area = create_area("primary rfs", &rfs_base, B_EXACT_ADDRESS,
		                               gKernelArgs->rootfs_image.size, B_ALREADY_WIRED,
		                               B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

		if (rfs_area < 0)
			panic("Can't convert primary RFS area");
	}

	if (gKernelArgs->secondary_rootfs_image.size > 0) {
		void *rfs_base = (void *) (uintptr_t) gKernelArgs->secondary_rootfs_image.start;
		area_id rfs_area = create_area("secondary rfs", &rfs_base, B_EXACT_ADDRESS,
		                               gKernelArgs->secondary_rootfs_image.size, B_ALREADY_WIRED,
		                               B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

		if (rfs_area < 0)
			panic("Can't convert secondary RFS area");
	}

	for (uint32_t i = 0; i < gKernelArgs->all_memory.num; ++i) {
		size_t pages_size;

		pages_size = sizeof(struct page) * (gKernelArgs->all_memory.ranges[i].size >> PAGE_SHIFT);
		pages_size = ROUND_PGUP(pages_size);

		sPageSegments[i]->pages_area_id = create_area("vm pages", (void **) &sPageSegments[i]->pages,
		                                              B_EXACT_ADDRESS, pages_size, B_ALREADY_WIRED,
		                                              B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);

		if (sPageSegments[i]->pages_area_id < 0) {
			panic("Can't convert page area");
		}
	}

#if defined(CONFIG_KDEBUGGER)
	debug_init_post_vm();

	add_debugger_command("page_stats", &dump_page_stats,
	                     "Dump statistics about page usage");
	add_debugger_command_etc("page", &dump_page,
	                         "Dump page info",
	                         "[ \"-p\" | \"-v\" ] [ \"-m\" ] <address>\n"
	                         "Prints information for the physical page. If neither \"-p\" nor\n"
	                         "\"-v\" are given, the provided address is interpreted as address of\n"
	                         "the vm_page data structure for the page in question. If \"-p\" is\n"
	                         "given, the address is the physical address of the page. If \"-v\" is\n"
	                         "given, the address is interpreted as virtual address in the current\n"
	                         "thread's address space and for the page it is mapped to (if any)\n"
	                         "information are printed. If \"-m\" is specified, the command will\n"
	                         "search all known address spaces for mappings to that page and print\n"
	                         "them.\n",
	                         0);
	add_debugger_command("page_queue", &dump_page_queue, "Dump page queue");
	add_debugger_command("find_page", &find_page,
	                     "Find out which queue a page is actually in");
#endif

	kernel_weak_ref_cache_init();

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	static_cast<VMKernelAddressSpace *>(VMAddressSpace::Kernel())->InitNonSecure();
#endif
}

int mem_reg() {
	return -EINVAL;
}

void vm_get_page_stats(struct page_stats *_pageStats) {
	sMemoryZone[0].get_page_stats(_pageStats);
}

void vm_get_page_stats_for_zone(struct page_stats *_pageStats, int zone) {
	ASSERT(zone >= 0 && zone < VM_ZONE_COUNT);
	sMemoryZone[zone].get_page_stats(_pageStats);
}

int vm_number_of_free_pages(void) {
	return __atomic_load_n(&sMemoryZone[0].fUnreservedFreePages, __ATOMIC_ACQUIRE) +
	       sMemoryZone[0].fCachedPageQueue.Count();
}

int vm_page_num_pages(void) {
	return sMemoryZone[0].fTotalPages - sIgnoredPages;
}

int vm_total_pages_count(void) {
	return sMemoryZone[0].fTotalPages;
}

void vm_page_get_stats(system_info *info) {
	// Note: there's no locking protecting any of the queues or counters here,
	// so we run the risk of getting bogus values when evaluating them
	// throughout this function. As these stats are for informational purposes
	// only, it is not really worth introducing such locking. Therefore we just
	// ensure that we don't under- or overflow any of the values.

	// The pages used for the block cache buffers. Those should not be counted
	// as used but as cached pages.
	// TODO: We should subtract the blocks that are in use ATM, since those
	// can't really be freed in a low memory situation.
	vm_pindex_t blockCachePages = 0;
	info->block_cache_pages = blockCachePages;

	// Non-temporary modified pages are special as they represent pages that
	// can be written back, so they could be freed if necessary, for us
	// basically making them into cached pages with a higher overhead. The
	// modified queue count is therefore split into temporary and non-temporary
	// counts that are then added to the corresponding number.
	vm_pindex_t modifiedNonTemporaryPages = (sModifiedPageQueue.Count() - __atomic_load_n(&sModifiedTemporaryPages, __ATOMIC_RELAXED));

	info->max_pages = sMemoryZone[0].fTotalPages;
	info->cached_pages = sMemoryZone[0].fCachedPageQueue.Count() + (uint64_t) modifiedNonTemporaryPages + blockCachePages;

#if defined(CONFIG_MEMORY_HOTPLUG)
	info->max_pages += sMemoryZone[1].fTotalPages;
	info->cached_pages += sMemoryZone[1].fCachedPageQueue.Count();
#endif

	// max_pages is composed of:
	//	active + inactive + unused + wired + modified + cached + free + clear
	// So taking out the cached (including modified non-temporary), free and
	// clear ones leaves us with all used pages.

	uint32_t subtractPages = info->cached_pages + sMemoryZone[0].fFreePageQueue.Count() +
	                         sMemoryZone[0].fClearPageQueue.Count();

#if defined(CONFIG_MEMORY_HOTPLUG)
	subtractPages += sMemoryZone[1].fFreePageQueue.Count() +
	                 sMemoryZone[1].fClearPageQueue.Count();
#endif

	info->used_pages = subtractPages > info->max_pages
	                           ? 0
	                           : info->max_pages - subtractPages;

	if (info->used_pages + info->cached_pages > info->max_pages) {
		// Something was shuffled around while we were summing up the counts.
		// Make the values sane, preferring the worse case of more used pages.
		info->cached_pages = info->max_pages - info->used_pages;
	}

	info->page_faults = vm_num_page_faults();
	info->ignored_pages = sIgnoredPages;

	// TODO: We don't consider pages used for page directories/tables yet.
}

void vm_get_info(system_info *info) {
	MutexLocker locker(sAvailableMemoryLock);

	info->needed_memory = sNeededMemory;
	info->free_memory = sAvailableMemory;
}

off_t vm_available_memory(void) {
	return sAvailableMemory;
}

off_t vm_total_memory(void) {
	return sTotalMemory;
}

off_t vm_available_not_needed_memory(void) {
	off_t result = 0;

	MutexLocker locker(sAvailableMemoryLock);
	result = sAvailableMemory - sNeededMemory;

	return result;
}

off_t vm_available_not_needed_memory_debug(void) {
	return sAvailableMemory - sNeededMemory;
}

size_t vm_kernel_address_space_left(void) {
	return VMAddressSpace::Kernel()->FreeSpace();
}

void vm_unreserve_memory(size_t amount) {
	MutexLocker locker(sAvailableMemoryLock);

	sAvailableMemory += amount;
}

int vm_try_reserve_memory(size_t amount, int priority, time_t timeout) {
	size_t reserve = kMemoryReserveForPriority[priority];

	MutexLocker locker(sAvailableMemoryLock);

	if (amount + reserve >= (size_t) ((sTotalMemory * 9) / 10)) {
		return -ENOMEM;
	}

	if (sAvailableMemory >= (off_t) (reserve + amount)) {
		sAvailableMemory -= amount;
		return 0;
	}

	if (timeout <= 0) {
		return -ENOMEM;
	}

	// turn timeout into an absolute timeout
	if (timeout == B_INFINITE_TIMEOUT || timeout < 0)
		timeout = B_INFINITE_TIMEOUT;
	else
		timeout += system_time();

	// loop until we've got the memory or the timeout occurs
	do {
		sNeededMemory += amount;

		// call the low resource manager
		locker.Unlock();
		low_resource(B_KERNEL_RESOURCE_MEMORY, sNeededMemory - sAvailableMemory,
		             B_ABSOLUTE_TIMEOUT | B_KILL_CAN_INTERRUPT,
		             timeout);
		locker.Lock();

		sNeededMemory -= amount;

		if (sAvailableMemory >= (off_t) (amount + reserve)) {
			sAvailableMemory -= amount;
			return 0;
		}

		// If thread has been killed, just stop waiting
		if (get_current_thread()->is_interrupted(B_KILL_CAN_INTERRUPT)) {
			return -ENOMEM;
		}
	} while (timeout > system_time());

	return -ENOMEM;
}

void __init_text vm_page_init_page_daemons(void) {
	for (int i = 0; i < VM_ZONE_COUNT; ++i) {
		char daemonName[B_OS_NAME_LENGTH];

		snprintf(daemonName, sizeof(daemonName), "page_daemon/%d", i);

		tid_t id = spawn_kernel_thread_etc(page_daemon, daemonName,
		                                   K_NORMAL_PRIORITY, (void *) (intptr_t) i, B_SYSTEM_TEAM);

		if (id < 0)
			panic("Can't spawn page daemon");

		resume_thread(id);
	}

	tid_t id = spawn_kernel_thread_etc(page_writer, "page_writer",
	                                   K_NORMAL_PRIORITY + 1, nullptr, B_SYSTEM_TEAM);
	if (id < 0)
		panic("Can't spawn page writer");
	resume_thread(id);
	id = spawn_kernel_thread_etc(page_scrubber, "page_scrubber",
	                             K_LOWEST_ACTIVE_PRIORITY, nullptr, B_SYSTEM_TEAM);
	if (id < 0)
		panic("Can't spawn page scrubber");
	resume_thread(id);
}

int vm_memset_physical(vm_paddr_t address, int value, vm_size_t length) {
	return sPhysicalPageMapper->MemsetPhysical(address, value, length);
}

int vm_memcpy_from_physical(void *to, vm_paddr_t from, size_t length, bool user) {
	return sPhysicalPageMapper->MemcpyFromPhysical(to, from, length, user);
}

int vm_memcpy_to_physical(vm_paddr_t to, const void *_from, size_t length, bool user) {
	return sPhysicalPageMapper->MemcpyToPhysical(to, _from, length, user);
}

void vm_memcpy_physical_page(vm_paddr_t to, vm_paddr_t from) {
	return sPhysicalPageMapper->MemcpyPhysicalPage(to, from);
}

int vm_get_physical_page(vm_paddr_t paddr, uintptr_t *_vaddr, void **_handle) {
	return sPhysicalPageMapper->GetPage(paddr, _vaddr, _handle);
}

int vm_put_physical_page(uintptr_t vaddr, void *handle) {
	return sPhysicalPageMapper->PutPage(vaddr, handle);
}

int vm_get_physical_page_current_cpu(vm_paddr_t paddr, uintptr_t *_vaddr, void **_handle) {
	return sPhysicalPageMapper->GetPageCurrentCPU(paddr, _vaddr, _handle);
}

int vm_put_physical_page_current_cpu(uintptr_t vaddr, void *handle) {
	return sPhysicalPageMapper->PutPageCurrentCPU(vaddr, handle);
}

void register_dynamic_memory_driver([[maybe_unused]] struct dynamic_memory_driver *driver) {
#if defined(CONFIG_MEMORY_HOTPLUG)
	sDynamicMemoryDriver = driver;
#endif
}

struct dynamic_memory_driver *get_dynamic_memory_driver(void) {
#if defined(CONFIG_MEMORY_HOTPLUG)
	return sDynamicMemoryDriver;
#else
	return nullptr;
#endif
}

void vm_free_kernel_args(void) {
	uint32_t i;

	gKernelArgs->preloaded_images = nullptr;
	gKernelArgs->kernel_image = nullptr;

	for (i = 0; i < gKernelArgs->kargs_memory.num; i++) {
		area_id area = vm_area_for(gKernelArgs->kargs_memory.ranges[i].start, true);
		if (area >= 0)
			delete_area(area);
	}
}

#if defined(CONFIG_KDEBUGGER)
int vm_get_physical_page_debug(vm_paddr_t paddr, uintptr_t *_vaddr, void **_handle) {
	return sPhysicalPageMapper->GetPageDebug(paddr, _vaddr, _handle);
}

int vm_put_physical_page_debug(uintptr_t vaddr, void *handle) {
	return sPhysicalPageMapper->PutPageDebug(vaddr, handle);
}
#endif
