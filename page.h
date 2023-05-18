
#ifndef __PAGE_H__
#define __PAGE_H__

#include <machine/param.h>

#define PAGE_MASK_LPAE           (~(((lpae_addr_t)PAGE_SIZE) - 1))

#ifndef __ASSEMBLY__
#ifdef __KERNEL__

#include <core/kconfig.h>
#include <core/types.h>
#include <core/atomic.h>
#include <core/spinlock.h>
#include <lib/stdatomic.h>
#include <core/queue.h>
#include <limits.h>
#include <trustware/vm.h>

#ifdef __cplusplus
#include <lib/DoublyLinkedList.h>
#include <lib/SplayTree.h>
#endif

enum {
	PAGE_STATE_ACTIVE = 0,
	PAGE_STATE_INACTIVE,
	PAGE_STATE_MODIFIED,
	PAGE_STATE_CACHED,
	PAGE_STATE_FREE,
	PAGE_STATE_CLEAR,
	PAGE_STATE_WIRED,
	PAGE_STATE_UNUSED,
	PAGE_STATE_COUNT,
	PAGE_STATE_FIRST_UNQUEUED = PAGE_STATE_WIRED
};

#define VM_PAGE_ALLOC_STATE	0x00000007
#define VM_PAGE_ALLOC_CLEAR	0x00000008
#define VM_PAGE_ALLOC_BUSY	0x00000010

#define PAGE_FLAG_CLEAR		VM_PAGE_ALLOC_CLEAR

/* Memory allocatable from external pool */
#define GFP_PRIO_EXTERNAL	3
/* Memory allocatable with highest priority */
#define GFP_PRIO_VIP		2
/* System level allocations - below VIP */
#define GFP_PRIO_SYSTEM		1
/* User level allocations - still leaving memory allocatable for SYSTEM and VIP levels */
#define GFP_PRIO_USER		0

#define GFP_PRIO_COUNT		4

#define VM_PAGE_RESERVE_USER		64
#define VM_PAGE_RESERVE_SYSTEM		8

#define VM_MEMORY_RESERVE_USER		(VM_PAGE_RESERVE_USER << PAGE_SHIFT)
#define VM_MEMORY_RESERVE_SYSTEM	(VM_PAGE_RESERVE_SYSTEM << PAGE_SHIFT)

struct MemorySegment;
struct task;
struct page;
struct ObjectCache;
struct VMCache;
struct VMArea;

struct VMCacheRef {
	struct VMCache * 	cache;
	int32_t				refcount;
};

#ifdef __cplusplus
struct vm_page_mapping;
using vm_page_mapping_link = DoublyLinkedListLink<vm_page_mapping>;

struct vm_page_mapping {
	vm_page_mapping_link 	page_link;
	vm_page_mapping_link 	area_link;
	struct page *			page;
	struct VMArea *			area;
};

class DoublyLinkedPageLink {
public:
	inline vm_page_mapping_link *operator()(vm_page_mapping *element) const {
		return &element->page_link;
	}

	inline const vm_page_mapping_link *operator()(
			const vm_page_mapping *element) const {
		return &element->page_link;
	}
};

class DoublyLinkedAreaLink {
public:
	inline vm_page_mapping_link *operator()(vm_page_mapping *element) const {
		return &element->area_link;
	}

	inline const vm_page_mapping_link *operator()(
			const vm_page_mapping *element) const {
		return &element->area_link;
	}
};

using vm_page_mappings = DoublyLinkedList<vm_page_mapping, DoublyLinkedPageLink>;
using vm_area_mappings = DoublyLinkedList<vm_page_mapping, DoublyLinkedAreaLink>;
#endif

typedef struct page * vm_page_t;

struct page {
	vm_pindex_t					pfn;
	uint8_t						state : 3;
	uint8_t						busy : 1;
	uint8_t						busy_writing : 1;
	uint8_t						modified : 1;
	uint8_t						accessed : 1;
#if defined(CONFIG_MEMORY_HOTPLUG)
	uint8_t						zone_index;
#endif
	uint8_t						usage_count;
	uint16_t					wired_count;
	uint16_t					tag;
	off_t						cache_offset;
	struct VMCacheRef *			cache_ref;
#if defined(CONFIG_MEMORY_HOTPLUG)
	struct MemorySegment *		segment;
#endif
#ifdef CONFIG_DEBUG
	void *						queue;
#endif
#ifdef __cplusplus
	vm_page_mappings			mappings;
	SplayTreeLink<page>			splay_tree_link;
	DoublyLinkedListLink<page>	queue_link;
	struct page *				cache_next;

	inline void Init(vm_pindex_t pageNumber);
	[[nodiscard]] inline VMCacheRef * CacheRef() const { return cache_ref; }
	inline void SetCacheRef(VMCacheRef * ref) { cache_ref = ref; }
	[[nodiscard]] inline VMCache * Cache() const { return cache_ref ? cache_ref->cache : nullptr; }
	inline void IncrementWiredCount();
	inline void DecrementWiredCount();
	[[nodiscard]] inline bool IsMapped() const { return wired_count > 0 || !mappings.IsEmpty(); }
	[[nodiscard]] inline uint16_t WiredCount() const { return wired_count; }
	[[nodiscard]] uint8_t State() const { return state; }
	void InitState(uint8_t newState);
	void SetState(uint8_t newState);
#if defined(CONFIG_MEMORY_HOTPLUG)
	uint8_t Zone() const { return zone_index; }
#else
#if __GNUC_PREREQ__(9, 0)
	constexpr uint8_t Zone() { return 0; }
#else
	static inline uint8_t Zone() { return 0; }
#endif
#endif
#endif
};

#define page_to_phys(_pg)	(((vm_paddr_t)page_to_pfn(_pg)) << PAGE_SHIFT)
#define phys_to_page(phys)	(pfn_to_page(((vm_paddr_t)(phys)) >> PAGE_SHIFT))

#define virt_to_phys(addr)	vm_virt_to_phys((void *)(addr))

struct vm_reservation_t {
#if defined(CONFIG_MEMORY_HOTPLUG)
	unsigned int counts[2];
#else
	unsigned int count;
#endif

#ifdef __cplusplus
	constexpr vm_reservation_t() :
#if defined(CONFIG_MEMORY_HOTPLUG)
		counts{0,0}
#else
		count{0}
#endif
	{
	}

	vm_reservation_t(const vm_reservation_t&) = delete;
	vm_reservation_t& operator = (const vm_reservation_t&) = delete;
#endif
};

#ifndef __cplusplus
typedef struct virtual_address_restrictions	virtual_address_restrictions;
typedef struct physical_address_restrictions physical_address_restrictions;
typedef struct vm_reservation_t vm_reservation_t;
#endif

struct virtual_address_restrictions {
	void*	address;
				// base or exact address, depending on address_specification
	uint32_t address_specification;
				// address specification as passed to create_area()
	size_t	alignment;
				// address alignment; overridden when
				// address_specification == B_ANY_KERNEL_BLOCK_ADDRESS
#ifdef __cplusplus
	constexpr virtual_address_restrictions() :
		address(nullptr),
		address_specification{0},
		alignment{0}
	{
	}
#endif
};


struct physical_address_restrictions {
	vm_paddr_t	low_address;
					// lowest acceptable address
	vm_paddr_t	high_address;
					// lowest no longer acceptable address; for ranges: the
					// highest acceptable non-inclusive end address
	vm_paddr_t	alignment;
					// address alignment
	vm_paddr_t	boundary;
					// multiples of which may not be crossed by the address
					// range

#ifdef __cplusplus
	constexpr physical_address_restrictions() :
		low_address(0),
		high_address(0),
		alignment(0),
		boundary(0)
	{
	}
#endif
};

__BEGIN_DECLS

vm_paddr_t vm_virt_to_phys(void *addr);
vm_page_t pfn_to_page(vm_pindex_t pageNumber);

static inline vm_pindex_t page_to_pfn(vm_page_t page) {
	return page->pfn;
}

void vm_page_unreserve_pages(vm_reservation_t* reservation);
int vm_reserve_pages(vm_reservation_t *reservation,
	uint32_t count, int priority, bool dontWait,
	bool oomKill);

vm_page_t vm_page_allocate_page_run(uint32_t flags,
		size_t length,
		const physical_address_restrictions* restrictions,
		int priority,
		vm_reservation_t * reservation);

void vm_init(void) __hidden;

struct page_stats {
	int32_t totalPages;
	int32_t	totalFreePages;
#ifdef CONFIG_DEBUG
	int32_t	lowestFreePages;
#endif
	int32_t	unsatisfiedReservations;
	int32_t	cachedPages;

#ifdef __cplusplus
	constexpr page_stats() :
        totalPages(0),
        totalFreePages(0),
#ifdef CONFIG_DEBUG
        lowestFreePages(0),
#endif
        unsatisfiedReservations(0),
        cachedPages(0)
    {
    }
#endif
};

void vm_get_page_stats(struct page_stats * _pageStats);
void vm_get_page_stats_for_zone(struct page_stats * _pageStats, int zone) __hidden;
int vm_page_count_min(void);
int vm_number_of_free_pages(void);
int vm_total_pages_count(void);
int vm_page_num_pages(void);

void arch_vm_page_copy(void * to, void * from);
void arch_vm_page_clear(void * to);

int vm_attach_memory_etc(vm_paddr_t phys, size_t size, int zone_index, void * driver_tag, uint32_t ns_tag);
int vm_attach_memory(vm_paddr_t phys, size_t size);
#if defined(CONFIG_MEMORY_HOTPLUG)
int vm_detach_memory_etc(vm_paddr_t phys, size_t size, void * driver_tag, uint32_t ns_tag);
#endif

extern bool vm_page_allocator_active;

unsigned int vm_num_page_faults(void);

off_t vm_available_memory(void);
off_t vm_total_memory(void);
off_t vm_available_not_needed_memory(void);
off_t vm_available_not_needed_memory_debug(void);
size_t vm_kernel_address_space_left(void);
void vm_unreserve_memory(size_t amount);
int vm_try_reserve_memory(size_t amount, int priority, time_t timeout);

extern int gMappedPagesCount;

struct page * vm_page_allocate_page_etc(vm_reservation_t * reservation, uint32_t flags);
void vm_page_free_etc(struct VMCache* cache, struct page * page, vm_reservation_t * reservation);
void vm_page_set_state(struct page *page, int pageState);
void vm_page_requeue(struct page *page, bool tail);
void vm_page_init_page_daemons(void);

int vm_memset_physical(vm_paddr_t address, int value, vm_size_t length);
int vm_memcpy_from_physical(void* to, vm_paddr_t from, size_t length, bool user);
int vm_memcpy_to_physical(vm_paddr_t to, const void* _from, size_t length, bool user);
void vm_memcpy_physical_page(vm_paddr_t to, vm_paddr_t from);
int vm_get_physical_page(vm_paddr_t paddr, uintptr_t* _vaddr, void** _handle);
int vm_put_physical_page(uintptr_t vaddr, void* handle);
int vm_get_physical_page_current_cpu(vm_paddr_t paddr, uintptr_t* _vaddr, void** _handle);
int vm_put_physical_page_current_cpu(uintptr_t vaddr, void* handle);

void vm_page_schedule_write_page_range(struct VMCache *cache, uint32_t firstPage, uint32_t endPage);

void vm_free_kernel_args(void);

extern off_t sAvailableMemory;
extern off_t sNeededMemory;

const char * page_state_to_string(int state);

#if defined(CONFIG_KDEBUGGER)
int vm_get_physical_page_debug(vm_paddr_t paddr, uintptr_t* _vaddr, void** _handle);
int vm_put_physical_page_debug(uintptr_t vaddr, void* handle);
#endif

//! Internal
void vm_init_mapper_early(struct kernel_args * args) __hidden;
void arch_vm_init_mapper_early(struct kernel_args * args) __hidden;

__END_DECLS

#ifdef __cplusplus
struct scoped_vm_reservation_t: vm_reservation_t {
	constexpr scoped_vm_reservation_t() = default;

	~scoped_vm_reservation_t() {
		vm_page_unreserve_pages(this);
	}
};
#endif

#endif /*!__ASSEMBLY_H__*/
#endif /* __KERNEL_H__*/
#endif /*!__PAGE_H__*/
