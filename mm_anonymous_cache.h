
#ifndef MM_MM_ANONYMOUS_CACHE_H_
#define MM_MM_ANONYMOUS_CACHE_H_

#include <core/module.h>
#include <trustware/system_info.h>
#ifdef __cplusplus
#include <mm/mm_cache.h>
#endif

#ifdef __cplusplus
using swap_addr_t = uint32_t;

struct VMAnonymousCache final : public VMCache {
	~VMAnonymousCache() override;

	int Init(bool canOvercommit, int32_t numPrecommittedPages,
			int32_t numGuardPages, uint32_t allocationFlags);

	int Resize(off_t newSize, int priority) override;
	int Rebase(off_t newBase, int priority) override;
	int Adopt(VMCache *source, off_t offset, off_t size, off_t newOffset) override;
	int Discard(off_t offset, off_t size) override;

	int Commit(off_t size, int priority) override;
	bool HasPage(off_t offset) override;
	bool CanWritePage(off_t offset) override;

	int Fault(struct VMAddressSpace *aspace, off_t offset) override;

	void Merge(VMCache* source) override;

	off_t Read(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes) override;
	off_t Write(off_t offset, const struct generic_iovec *vecs, uint32_t flags, size_t count,
			size_t  *_numBytes) override;
	int WriteAsync(off_t offset, const struct generic_iovec* vecs, size_t count,
			size_t numBytes, uint32_t flags, AsyncIOCallback* callback) override;

	[[nodiscard]] int MaxPagesPerAsyncWrite() const override;
	[[nodiscard]] int MaxPagesPerWrite() const override;

#if defined(CONFIG_KDEBUGGER)
	bool DebugHasPage(off_t offset) final;
#endif

	friend bool swap_free_page_swap_space(struct page* page);
protected:
	void DeleteObject() override;

private:
	void _SwapBlockBuild(off_t pageIndex, swap_addr_t slotIndex, uint32_t count);
	void _SwapBlockFree(off_t pageIndex, uint32_t count);
	swap_addr_t _SwapBlockGetAddress(off_t pageIndex);
	int _Commit(off_t size, int priority);

	void _MergePagesSmallerSource(VMAnonymousCache* sourceToMerge);
	void _MergePagesSmallerConsumer(VMAnonymousCache* sourceToMerge);
	void _MergeSwapPages(VMAnonymousCache* sourceToMerge);
	void _FreeSwapPageRange(off_t fromOffset, off_t toOffset,
			bool skipBusyPages = true);

private:
	class WriteCallback;
	friend class WriteCallback;

	bool			can_overcommit;
	bool			has_precommitted;
	uint32_t		precommitted_pages;
	int32_t			guard_size;
	off_t			committed_swap_size;
	off_t			allocated_swap_size;
};

void swap_get_info(system_info* info);
int swap_file_delete(const char* path);
int swap_file_add(const char* path);
void swap_init();
bool swap_free_page_swap_space(struct page* page);
#endif

#endif /* MM_MM_ANONYMOUS_CACHE_H_ */
