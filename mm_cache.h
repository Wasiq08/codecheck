
#ifndef CORE_MM_CACHE_H_
#define CORE_MM_CACHE_H_

#include <core/kconfig.h>
#include <mm/page.h>
#include <mm/VMArea.h>
#include <core/mutex.h>
#include <lib/SplayTree.h>
#include <lib/DoublyLinkedList.h>
#include <boost/intrusive/list.hpp>
#include <mm/vm_cache_lock.h>

struct VMCachePagesTreeDefinition {
	using KeyType = off_t;
	using NodeType = struct page;

	[[nodiscard]] constexpr static inline KeyType GetKey(const NodeType *node) {
		return node->cache_offset;
	}

	[[nodiscard]] constexpr static inline SplayTreeLink<NodeType>* GetLink(
			NodeType *node) {
		return &node->splay_tree_link;
	}

	[[nodiscard]] constexpr static inline int Compare(KeyType key,
			const NodeType *node) {
		return key == node->cache_offset ?
				0 : (key < node->cache_offset ? -1 : 1);
	}

	[[nodiscard]] constexpr static inline NodeType** GetListLink(
			NodeType *node) {
		return &node->cache_next;
	}
};

using VMCachePagesTree = IteratableSplayTree<VMCachePagesTreeDefinition>;

struct VMCache;
struct VMArea;
struct thread;
struct vfs_iovec;
struct vnode;
class AsyncIOCallback;

enum {
	PAGE_EVENT_NOT_BUSY = 1
};

struct VMCache : public DoublyLinkedListLinkImpl<VMCache> {
	using ConsumerList = DoublyLinkedList<VMCache>;

	struct PageEventWaiter: public boost::intrusive::list_base_hook<
			boost::intrusive::link_mode<boost::intrusive::safe_link>> {
	private:
		cond_var_t waiterCond{"page_event"};
		cond_var_entry waiterEntry;
		struct page *page;
		uint32_t events;

	public:
		PageEventWaiter(struct page * p, uint32_t eventMask) noexcept :
			page(p),
			events(eventMask)
		{
			waiterCond.Add(&waiterEntry);
		}

#ifdef CONFIG_DEBUG
		~PageEventWaiter() noexcept {
			KASSERT(!this->is_linked(), ("PageEventWaiter still linked"));
		}
#endif

		void WakeUp(int reason = 0) noexcept {
			waiterCond.NotifyAll(reason);
		}

		struct page * get_page() const noexcept {
			return page;
		}

		uint32_t get_events() const noexcept {
			return events;
		}

		void Wait() noexcept {
			waiterEntry.Wait();
		}
	};

	using PageEventWaiterList = boost::intrusive::list<PageEventWaiter,
			boost::intrusive::constant_time_size<false>>;

	ConsumerList			consumers;
	struct VMArea *			areas = nullptr;
	VMCachePagesTree		pages;
	VMCache *				source = nullptr;
	off_t					virtual_base = 0;
	off_t					virtual_end = 0;
	off_t					virtual_last_page = 0;
	off_t					committed_size = 0;
	uint32_t				page_count = 0;
	bool	 				temporary = false;
	bool					virtual_cache = false;
	bool					mergeable = false;
	uint8_t	 				type = 0;
	int32_t					ref_count = 1;
	VMCacheLock				lock;
	void *					user_data = nullptr;
	VMCacheRef *			cache_ref = nullptr;
	PageEventWaiterList		page_event_waiters{};
	size_t					wired_pages_count = 0;

	VMCache() = default;
	virtual ~VMCache();
	int Init(uint32_t cacheType, int allocationFlags);

	virtual void Delete();

	inline bool Lock();
	inline bool TryLock();
	inline bool SwitchLock(VMCacheLock* from);
	inline bool SwitchFromReadLock(rw_lock_t* from);
	void Unlock(bool consumerLocked = false);
	inline void AssertLocked();

	inline void AcquireRefLocked();
	inline void AcquireRef();
	inline void ReleaseRefLocked();
	inline void ReleaseRef();
	inline void ReleaseRefAndUnlock(bool consumerLocked = false);

	[[nodiscard]] inline VMCacheRef* CacheRef() const { return this->cache_ref; }

	[[nodiscard]] struct page * LookupPage(off_t offset);
	void InsertPage(struct page * page, off_t offset);
	void RemovePage(struct page * page);
	void MovePage(struct page * page);
	void MovePage(struct page * page, off_t offset);
	void MoveAllPages(VMCache* fromCache);
	[[nodiscard]] struct page * FirstPage(off_t offset, bool greater, bool orEqual);

	[[nodiscard]] virtual int32_t GuardSize() { return 0; }

	void AddConsumer(VMCache* consumer);
	void InsertAreaLocked(VMArea* area);
	void RemoveArea(VMArea* area);
	void TransferAreas(VMCache* fromCache);
	[[nodiscard]] uint32_t CountWritableAreas(VMArea* ignoreArea) const;

	int WriteModified();
	int SetMinimalCommitment(off_t commitment, int priority);
	virtual int Resize(off_t newSize, int priority);
	virtual int Rebase(off_t newBase, int priority);
	virtual int Adopt(VMCache *sourceToAdopt, off_t offset, off_t size,
	                  off_t newOffset);
	virtual int Discard(off_t offset, off_t size);

	int FlushAndRemoveAllPages();

	[[nodiscard]] inline void* UserData() const { return user_data; }
	inline void SetUserData(void* data) { user_data = data; }
	[[nodiscard]] inline int32_t RefCount() const { return ref_count; }

	virtual int Commit(off_t size, int priority);
	virtual bool HasPage(off_t offset);
	virtual bool CanWritePage(off_t offset);

	[[nodiscard]] virtual int Fault(struct VMAddressSpace *aspace, off_t offset);

	virtual void Merge(VMCache* sourceToMerge);

	virtual int AcquireUnreferencedStoreRef();
	virtual void AcquireStoreRef();
	virtual void ReleaseStoreRef();

	[[nodiscard]] virtual off_t Read(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes);
	[[nodiscard]] virtual off_t Write(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes);
	[[nodiscard]] virtual int WriteAsync(off_t offset, const struct generic_iovec* vecs, size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* callback);

	[[nodiscard]] virtual int MaxPagesPerAsyncWrite() const { return 1; }
	[[nodiscard]] virtual int MaxPagesPerWrite() const { return -1; }

	void WaitForPageEvents(struct page * page, uint32_t events, bool relock);

	inline void NotifyPageEvents(struct page * page, uint32_t events) {
		if (!page_event_waiters.empty()) {
			_NotifyPageEvents(page, events);
		}
	}

	void MarkPageUnbusy(struct page * page);

	static void MarkDeletedForVFS(VMCache * cache);

	void IncrementWiredPagesCount();
	void DecrementWiredPagesCount();

#if defined(CONFIG_KDEBUGGER)
	inline struct page *DebugLookupPage(off_t offset) {
		return pages.Lookup(offset >> PAGE_SHIFT);
	}

	virtual bool DebugHasPage(off_t offset) {
		return HasPage(offset);
	}

	virtual	void Dump(bool showPages) const;
#endif

protected:
	virtual void DeleteObject() = 0;

private:
	[[nodiscard]] inline bool _IsMergeable() const;

	void _MergeWithOnlyConsumer();
	void _RemoveConsumer(VMCache* consumer);
	void _NotifyPageEvents(struct page * page, uint32_t events);
	bool _FreePageRange(VMCachePagesTree::Iterator it, vm_pindex_t* toPage = nullptr);
};


class VMAnonymousNoSwapCache final : public VMCache {
public:
	~VMAnonymousNoSwapCache() override;

	int Init(bool canOvercommit, int32_t numPrecommittedPages,
			int32_t numGuardPages, int allocationFlags);

	int Commit(off_t size, int priority) override;
	bool HasPage(off_t offset) override;

	int32_t GuardSize() override { return fGuardedSize; }
	int Fault(struct VMAddressSpace* aspace, off_t offset) override;

protected:
	void DeleteObject() override;

private:
	bool 		fCanOvercommit;
	bool 		fHasPrecommitted;
	uint8_t 	fPrecommittedPages;
	int32_t 	fGuardedSize;
};

class VMDeviceCache final: public VMCache {
public:
	int Init(int allocationFlags);

protected:
	void DeleteObject() final;
};

class VMNullCache final : public VMCache {
public:
	int Init(int allocationFlags);

protected:
	void DeleteObject() final;
};

class VMNonSecureCache final: public VMCache {
public:
	int Init(int allocationFlags);

protected:
	void DeleteObject() final;
};

struct VMVnodeCache final : public VMCache {
	int Init(struct vnode * vnode, int allocationFlags);

	bool HasPage(off_t offset) override;
	bool CanWritePage(off_t offset) override;

	off_t Read(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes) override;
	off_t Write(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes) override;
	int WriteAsync(off_t offset, const struct generic_iovec* vecs, size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* callback) override;

	int Fault(struct VMAddressSpace *aspace, off_t offset) override;

	int AcquireUnreferencedStoreRef() override;
	void AcquireStoreRef() override;
	void ReleaseStoreRef() override;

	[[nodiscard]] inline dev_t DeviceId() const { return fDevice; }
	[[nodiscard]] inline ino_t InodeId() const { return fInode; }

protected:
		void DeleteObject() final;

private:
	friend struct VMCache;

	struct vnode *			fVnode;
	ino_t					fInode;
	dev_t					fDevice;
	bool					fVnodeDeleted;
};

inline bool VMCache::Lock()
{
	return lock.Lock() == 0;
}

inline bool VMCache::TryLock()
{
	return lock.TryLock();
}

inline bool VMCache::SwitchLock(VMCacheLock * from)
{
	return lock.SwitchFromLock(*from) == 0;
}

inline bool VMCache::SwitchFromReadLock(rw_lock_t * from)
{
	return lock.SwitchFromReadLock(*from) == 0;
}

inline void VMCache::AssertLocked()
{
	lock.AssertLocked();
}

inline void VMCache::AcquireRefLocked()
{
	ref_count++;
}

inline void VMCache::AcquireRef()
{
	Lock();
	ref_count++;
	Unlock();
}

inline void VMCache::ReleaseRefLocked()
{
	ref_count--;
}

void VMCache::ReleaseRef()
{
	Lock();
	ref_count--;
	Unlock();
}

inline void VMCache::ReleaseRefAndUnlock(bool consumerLocked)
{
	ReleaseRefLocked();
	Unlock(consumerLocked);
}

inline void VMCache::IncrementWiredPagesCount() {
	ASSERT(wired_pages_count < page_count);
	wired_pages_count++;
}

inline void VMCache::DecrementWiredPagesCount() {
	ASSERT(wired_pages_count > 0);
	wired_pages_count--;
}

inline void page::IncrementWiredCount() {
	if (wired_count++ == 0) {
		if(cache_ref) {
			cache_ref->cache->IncrementWiredPagesCount();
		}
	}
}

inline void page::DecrementWiredCount() {
	ASSERT(wired_count > 0);
	if (--wired_count == 0) {
		if(cache_ref) {
			cache_ref->cache->DecrementWiredPagesCount();
		}
	}
}

class VMCacheFactory {
public:
	static int CreateAnonymousCache(VMCache*& cache, bool canOvercommit,
			int32_t numPrecommittedPages, int32_t numGuardPages, bool swappable,
			int priority, uint32_t allocationFlags);
	static int CreateDeviceCache(VMCache*& cache, uint32_t allocationFlags);
	static int CreateNullCache(int priority, VMCache*& cache, uint32_t allocationFlags);
	static int CreateNonSecureCache(VMCache*& cache, uint32_t allocationFlags);
	static int CreateVnodeCache(VMCache*& cache, struct vnode* vnode, uint32_t allocationFlags);
};

struct ObjectCache;

extern ObjectCache* gCacheRefObjectCache;
extern ObjectCache* gVMCacheObjectCache;

#endif /* CORE_MM_CACHE_H_ */
