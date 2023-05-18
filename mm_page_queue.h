
#ifndef TRUSTZONE_TEE_TRUSTZONE_TEE_SRC_SOURCE_KERNEL_CORE_MM_PAGE_QUEUE_H_
#define TRUSTZONE_TEE_TRUSTZONE_TEE_SRC_SOURCE_KERNEL_CORE_MM_PAGE_QUEUE_H_

#include <mm/page.h>
#include <lib/AutoLock.h>
#include <lib/cxxabi.h>

struct VMPageQueue {
public:
	typedef DoublyLinkedList<struct page,
		DoublyLinkedListMemberGetLink<struct page, &page::queue_link> > PageList;

	using Iterator = PageList::ConstIterator;

public:
			VMPageQueue() = default;

			VMPageQueue(const char * name) {
				Init(name);
			}

			void				Init(const char* name) {
				fName = name;
			}

			[[nodiscard]] const char*			Name() const	{ return fName; }

	inline	void				Append(struct page * page);
	inline	void				Prepend(struct page* page);
	inline	void				InsertAfter(struct page* insertAfter, struct page* page);
	inline	void				Remove(struct page* page);
	[[nodiscard]] inline  struct page*		RemoveHead();
	inline	void				Requeue(struct page* page, bool tail);

	inline	void				AppendUnlocked(struct page* page);
	inline	void				AppendUnlocked(PageList& pages, uint32_t count);
	inline	void				PrependUnlocked(struct page* page);
	inline	void				RemoveUnlocked(struct page* page);
	[[nodiscard]] inline	struct page*		RemoveHeadUnlocked();
	inline	void				RequeueUnlocked(struct page* page, bool tail);

	[[nodiscard]] inline	struct page*		Head() const;
	[[nodiscard]] inline	struct page*		Tail() const;
	[[nodiscard]] inline	struct page*		Previous(struct page* page) const;
	[[nodiscard]] inline	struct page*		Next(struct page* page) const;

	[[nodiscard]] inline	size_t				Count() const	{ return fCount; }

	[[nodiscard]] inline	Iterator			GetIterator() const;

	[[nodiscard]] inline	spinlock&			GetLock()	{ return fLock; }

protected:
			const char*			fName{"page queue"};
			spinlock			fLock{"page queue"};
			size_t				fCount{0};
			PageList			fPages{};
};

void VMPageQueue::Append(struct page * page)
{
#ifdef CONFIG_DEBUG
	if (page->queue != nullptr) {
		panic("%p->VMPageQueue::Append(page: %p): page thinks it is "
			"already in queue %p", this, page, page->queue);
	}
#endif	// DEBUG_PAGE_QUEUE

	fPages.Add(page);
	fCount++;

#ifdef CONFIG_DEBUG
	page->queue = this;
#endif
}


void
VMPageQueue::Prepend(struct page * page)
{
#ifdef CONFIG_DEBUG
	if (page->queue != nullptr) {
		panic("%p->VMPageQueue::Prepend(page: %p): page thinks it is "
			"already in queue %p", this, page, page->queue);
	}
#endif	// DEBUG_PAGE_QUEUE

	fPages.Add(page, false);
	fCount++;

#ifdef CONFIG_DEBUG
	page->queue = this;
#endif
}


void
VMPageQueue::InsertAfter(struct page* insertAfter, struct page* page)
{
#ifdef CONFIG_DEBUG
	if (page->queue != nullptr) {
		panic("%p->VMPageQueue::InsertAfter(page: %p): page thinks it is "
			"already in queue %p", this, page, page->queue);
	}
#endif	// DEBUG_PAGE_QUEUE

	fPages.InsertAfter(insertAfter, page);
	fCount++;

#ifdef CONFIG_DEBUG
	page->queue = this;
#endif
}


void
VMPageQueue::Remove(struct page* page)
{
#ifdef CONFIG_DEBUG
	if (page->queue != this) {
		panic("%p->VMPageQueue::Remove(page: %p): page thinks it "
			"is in queue %p", this, page, page->queue);
	}
#endif	// DEBUG_PAGE_QUEUE

	fPages.Remove(page);
	fCount--;

#ifdef CONFIG_DEBUG
	page->queue = nullptr;
#endif
}


struct page*
VMPageQueue::RemoveHead()
{
	struct page* page = fPages.RemoveHead();
	if (page != nullptr) {
		fCount--;

#ifdef CONFIG_DEBUG
		if (page->queue != this) {
			panic("%p->VMPageQueue::RemoveHead(): page %p thinks it is in "
				"queue %p", this, page, page->queue);
		}

		page->queue = nullptr;
#endif	// DEBUG_PAGE_QUEUE
	}

	return page;
}


void
VMPageQueue::Requeue(struct page* page, bool tail)
{
#ifdef CONFIG_DEBUG
	if (page->queue != this) {
		panic("%p->VMPageQueue::Requeue(): page %p thinks it is in "
			"queue %p", this, page, page->queue);
	}
#endif

	fPages.Remove(page);
	fPages.Add(page, tail);
}


void
VMPageQueue::AppendUnlocked(struct page* page)
{
	InterruptsSpinLocker locker(fLock);
	Append(page);
}


void
VMPageQueue::AppendUnlocked(PageList& pages, uint32_t count)
{
#ifdef CONFIG_DEBUG
	for (PageList::Iterator it = pages.GetIterator();
			struct page* page = it.Next();) {
		if (page->queue != nullptr) {
			panic("%p->VMPageQueue::AppendUnlocked(): page %p thinks it is "
				"already in queue %p", this, page, page->queue);
		}

		page->queue = this;
	}

#endif	// DEBUG_PAGE_QUEUE

	InterruptsSpinLocker locker(fLock);

	fPages.MoveFrom(&pages);
	fCount += count;
}


void
VMPageQueue::PrependUnlocked(struct page* page)
{
	InterruptsSpinLocker locker(fLock);
	Prepend(page);
}


void
VMPageQueue::RemoveUnlocked(struct page* page)
{
	InterruptsSpinLocker locker(fLock);
	return Remove(page);
}


struct page*
VMPageQueue::RemoveHeadUnlocked()
{
	InterruptsSpinLocker locker(fLock);
	return RemoveHead();
}


void
VMPageQueue::RequeueUnlocked(struct page* page, bool tail)
{
	InterruptsSpinLocker locker(fLock);
	Requeue(page, tail);
}


struct page*
VMPageQueue::Head() const
{
	return fPages.Head();
}


struct page*
VMPageQueue::Tail() const
{
	return fPages.Tail();
}


struct page*
VMPageQueue::Previous(struct page* page) const
{
	return fPages.GetPrevious(page);
}


struct page*
VMPageQueue::Next(struct page* page) const
{
	return fPages.GetNext(page);
}


VMPageQueue::Iterator
VMPageQueue::GetIterator() const
{
	return fPages.GetIterator();
}


#endif /* TRUSTZONE_TEE_TRUSTZONE_TEE_SRC_SOURCE_KERNEL_CORE_MM_PAGE_QUEUE_H_ */
