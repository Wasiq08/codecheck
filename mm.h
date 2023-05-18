#ifndef __MM_H__
#define __MM_H__

#include <mm/page.h>
#include <core/arch_mm.h>

#ifndef __ASSEMBLY__
#include <core/debug.h>
#include <core/atomic.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/usercopy.h>
#include <mm/vm_pmap.h>
#include <core/condvar.h>
#include <core/mutex.h>
#include <trustware/vm.h>
#include <sys/mman.h>
#ifdef __cplusplus
#include <lib/OpenHashTable.h>
#include <lib/DoublyLinkedList.h>
#include <lib/SinglyLinkedList.h>
#endif
#endif

#define THREAD_STACK_ORDER	  (12)
#define THREAD_STACK_SIZE	  (PAGE_SIZE << THREAD_STACK_ORDER)
#define THREAD_INITIAL_STACK_SIZE (PAGE_SIZE << 2)
#define THREAD_STACK_SIZE_MAX (PAGE_SIZE * (0x1 << (THREAD_STACK_ORDER+1)))

// 64kB or less free RAM
#define VM_LOWMEM_THRESHOLD	0x10000

/////////////////// USERSPACE INTERFACE END

/* Basic vma flags (vma->flags) */
#define VMA_FLAG_U_READ		B_READ_AREA
#define VMA_FLAG_U_WRITE	B_WRITE_AREA
#define VMA_FLAG_U_EXECUTE	B_EXECUTE_AREA
#define VMA_FLAG_U_MASK		(B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA)
#define VMA_FLAG_S_READ		(B_KERNEL_READ_AREA)
#define VMA_FLAG_S_WRITE	(B_KERNEL_WRITE_AREA)
#define VMA_FLAG_S_EXECUTE	(B_KERNEL_EXECUTE_AREA)
#define VMA_FLAG_S_MASK		(B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_KERNEL_EXECUTE_AREA)
#define VMA_FLAG_PERM_MASK	(VMA_FLAG_S_MASK | VMA_FLAG_U_MASK)
#define VMA_FLAG_NONSECURE	B_NONSECURE_AREA
#define VMA_FLAG_SHARED		B_SHARED_AREA
#define VMA_FLAG_DEVICE		B_DEVICE_AREA

/* extended flags */
#define VMA_EFLAG_USER_CLONE B_USER_CLONEABLE_AREA /* Kernel area which can be cloned to user space */
#define VMA_EFLAG_POPULATE	(1u << 15) /* ?? */
#define VMA_EFLAG_NODUMP	B_NODUMP_AREA	 /* Don't dump VMA in case of crash */
#define VMA_EFLAG_STACK		B_STACK_AREA	 /* This is stack mapping */

/* cache mode - valid only for memory mappings */
#define VMA_EFLAG_CACHE_NORMAL	(0x000000)	/* Normal ram mapping*/
#define VMA_EFLAG_CACHE_SO	(0x100000)	/* Strongly Ordered */
#define VMA_EFLAG_CACHE_WC	(0x200000)	/* Write Combine */
#define VMA_EFLAG_CACHE_WT	(0x300000)	/* Write Through, no Write Allocate */
#define VMA_EFLAG_CACHE_WB	(0x400000)	/* Write Back, no Write Allocate */
#define VMA_EFLAG_CACHE_WBWA	(0x500000)	/* Write Back, Write Allocate */
#define VMA_EFLAG_CACHE_NC	(0x600000)      /* Non Cacheable */
#define VMA_EFLAG_CACHE_MASK	(0xF00000)

/* Predefined VMA flags (for specific mappings) */
#define VMA_TYPE_U_TEXT		(B_READ_AREA | B_EXECUTE_AREA | B_KERNEL_READ_AREA)
#define VMA_TYPE_U_DATA		(B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_U_STACK	(B_STACK_AREA | B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_U_WSM		(B_NONSECURE_AREA | B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_U_SHARED	(B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_U_DEVICE	(B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_DEVICE_AREA)

#define VMA_TYPE_S_TEXT		(B_KERNEL_READ_AREA | B_KERNEL_EXECUTE_AREA)
#define VMA_TYPE_S_DATA		(B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_S_STACK	(B_KERNEL_STACK_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_S_WSM		(B_NONSECURE_AREA | B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_S_SHARED	(B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA)
#define VMA_TYPE_S_DEVICE	(B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_DEVICE_AREA)

/* Compat flags */
#define VM_COMPAT_FLAGS

#ifdef VM_COMPAT_FLAGS
#define VM_ATTR_NONE            (0u)
#define VM_ATTR_READ            VMA_FLAG_S_READ
#define VM_ATTR_WRITE           VMA_FLAG_S_WRITE
#define VM_ATTR_EXECUTE         VMA_FLAG_S_EXECUTE
#define VM_ATTR_NONSECURE       VMA_FLAG_NONSECURE
#define VM_ATTR_COHERENT        VMA_FLAG_SHARED

#define VM_PROT_NONE            VM_ATTR_NONE
#define VM_PROT_READ            VM_ATTR_READ
#define VM_PROT_WRITE           VM_ATTR_WRITE
#define VM_PROT_EXECUTE         VM_ATTR_EXECUTE
#define VM_PROT_NONSECURE       VM_ATTR_NONSECURE
#define VM_PROT_SHARED          VM_ATTR_COHERENT
#define VM_PROT_MASK            VMA_FLAG_PERM_MASK

#define VMA_TYPE_TEXT           VMA_TYPE_S_TEXT
#define VMA_TYPE_DATA           VMA_TYPE_S_DATA
#define VMA_TYPE_STACK          VMA_TYPE_S_STACK
#define VMA_TYPE_WSM            VMA_TYPE_S_WSM
#define VMA_TYPE_SHARED         VMA_TYPE_S_SHARED
#endif

#ifndef __ASSEMBLY__

enum {
	CACHE_TYPE_RAM = 0,
	CACHE_TYPE_VNODE,
	CACHE_TYPE_NULL,
	CACHE_TYPE_DEVICE,
	CACHE_TYPE_NONSECURE,
	CACHE_TYPE_NONSECURE_DEVICE,
	CACHE_TYPE_PFNMAP,
	CACHE_TYPE_NONSECURE_PFNMAP
};

struct VMArea;
struct VMCache;

// vm_area_operations->fault should return this so that system
// uses default handler
#define VM_FAULT_DONT_HANDLE		(-666)

#define VM_RESERVED_AREA_ID			(-42)
#define RESERVED_AVOID_BASE			1

struct vm_fault_info;
struct VMTranslationMap;
struct VMAddressSpace;
struct generic_iovec;

struct task;
struct thread;

// area creation flags
#define CREATE_AREA_DONT_WAIT			0x01
#define CREATE_AREA_UNMAP_ADDRESS_RANGE	0x02
#define CREATE_AREA_DONT_CLEAR			0x04
#define CREATE_AREA_PRIORITY_VIP		0x08
#define CREATE_AREA_DONT_COMMIT_MEMORY	0x10

__BEGIN_DECLS

void* uva_map_pages(vm_paddr_t phyaddr,size_t size);
int uva_unmap_pages(vaddr_t start,size_t size);
int uvm_unmap_user_memory(uint32_t pid, uintptr_t address, size_t size);

// Physical address
void* get_phy_to_vir(vm_paddr_t phyaddr,size_t size);
void *kva_map_pages(vm_paddr_t start, vm_size_t size, vma_flags_t flags);
void *kva_map_pages_named(vm_paddr_t start, vm_size_t size, vma_flags_t flags, const char * name);
void kva_unmap_pages(unsigned long start, unsigned int size);
void *kva_map_pfns(vm_pindex_t pfns[], unsigned int nr, vma_flags_t flags);

int vm_create_anonymous_area(struct VMAddressSpace * vms,
				const virtual_address_restrictions * addressRestrictions,
				size_t length,
				vma_flags_t flags,
				vaddr_t * p_result_address,
				struct VMArea ** p_result_vma,
				const char * area_name);

int vm_create_file_mapping(struct VMAddressSpace * vms,
				struct vfs_context * ctx,
				int file_handle,
				const virtual_address_restrictions * addressRestrictions,
				size_t length,
				vma_flags_t flags,
				off_t offset,
				vaddr_t * p_result_address,
				struct VMArea ** p_vma);

int vm_create_null_mapping(struct VMAddressSpace * vms,
			const virtual_address_restrictions * addressRestrictions,
			size_t length,
			vaddr_t * p_result_address,
			struct VMArea ** p_result_vma,
			const char * area_name);

int vm_create_stack_area(struct VMAddressSpace * vms,
				const virtual_address_restrictions * addressRestrictions,
				size_t length,
				size_t max_length,
				vaddr_t * p_result_address,
				struct VMArea ** p_result_vma,
				bool kernel_stack);

vma_flags_t vm_map_user_prot(int prot);

void *kva_map_nonsecure_memory_to_kernel(uint64_t virt, size_t length);

void * kva_get_nonsecure_remap_slot(int slot, vm_paddr_t address) __hidden;

bool arch_vm_supports_protection(vma_flags_t protection);
int vm_delete_area(uint32_t team, int id, bool kernel);
int vm_area_for(uintptr_t address, bool kernel);

int vm_map_physical_memory(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t size, vma_flags_t protection,
	vm_paddr_t physicalAddress, bool alreadyWired);
int vm_map_physical_memory_vecs(pid_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t* _size, uint32_t protection,
	const struct generic_iovec* vecs, uint32_t vecCount);
int vm_create_null_area(uint32_t team, const char* name, vaddr_t * address,
	uint32_t addressSpec, size_t size, uint32_t flags);
int vm_copy_area(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, area_id sourceID);
int vm_map_physical_memory_pfns(uint32_t team, const char* name, vaddr_t * _address,
	uint32_t addressSpec, size_t countPfns, vma_flags_t protection,
	const vm_pindex_t * pfnArray);
int vm_create_anonymous_area_etc(pid_t team, const char *name, size_t size,
	uint32_t wiring, uint32_t protection, uint32_t flags, size_t guardSize,
	const virtual_address_restrictions* virtualAddressRestrictions,
	const physical_address_restrictions* physicalAddressRestrictions,
	bool kernel, vaddr_t* _address);
int vm_page_fault(uintptr_t address, bool isWrite, bool isExecute, bool isUser);
int vm_set_area_protection(pid_t team, int areaID, uint32_t newProtection, bool kernel);
int vm_map_file(pid_t aid, const char* name, vaddr_t * address, uint32_t addressSpec,
	size_t size, uint32_t protection, bool privateMapping, bool unmapAddressRange,
	int fd, off_t offset);
int user_map_file(const char* name, vaddr_t * _address, uint32_t addressSpec,
	size_t size, uint32_t protection, bool privateMapping, bool unmapAddressRange,
	int fd, off_t offset);

#ifdef __cplusplus
}
struct VMPageWiringInfo;
int vm_wire_page(pid_t team, uintptr_t address, bool writable, struct VMPageWiringInfo* info);
void vm_unwire_page(struct VMPageWiringInfo* info);
area_id transfer_area(area_id id, void** _address, uint32_t addressSpec, pid_t target, bool kernel);

extern "C" {
#endif

int lock_memory_etc(pid_t team, void* address, size_t numBytes, bool writable);
int lock_memory(void* address, size_t numBytes, bool writable);
int unlock_memory_etc(pid_t team, void* address, size_t numBytes, bool writable);
int unlock_memory(void* address, size_t numBytes, bool writable);

int vm_clone_area(pid_t team, const char* name, vaddr_t * address,
	uint32_t addressSpec, uint32_t protection, bool privateMapping, int sourceID,
	bool kernel);

void vm_page_write_single_page(struct VMCache* cache, struct page * page);
int vm_page_write_modified_pages(struct VMCache *cache);
int vm_page_write_modified_page_range(struct VMCache* cache, off_t firstPage, off_t endPage);

typedef struct {
	vm_paddr_t	address;	/* address in physical memory */
	vm_size_t	size;		/* size of block */
} physical_entry;

int get_memory_map_etc(pid_t team, const void* address, size_t numBytes, physical_entry * table, uint32_t * _numEntries);
int32_t get_memory_map(const void* address, size_t numBytes, physical_entry* table, int32_t numEntries);

struct VMCache* vm_cache_acquire_locked_page_cache(struct page * page, bool dontWait);
struct VMCache* vm_area_get_locked_cache(struct VMArea * area);
void vm_area_put_locked_cache(struct VMCache* cache);
bool vm_test_map_modification(struct page* page);
void vm_clear_map_flags(struct page * page, uint32_t flags);
int32_t vm_clear_page_mapping_accessed_flags(struct page *page);
int32_t vm_remove_all_page_mappings_if_unaccessed(struct page *page);

int vm_unreserve_address_range(pid_t team, vaddr_t address, size_t size);
int vm_reserve_address_range(pid_t team, vaddr_t *_address,
		uint32_t addressSpec, size_t size, uint32_t flags, size_t alignment);

void vm_remove_all_page_mappings(struct page * page);

int arch_vm_cache_control(uintptr_t base, size_t size, vm_paddr_t phys, int mode);

vm_paddr_t vm_allocate_early_physical_page(void) __hidden;
uintptr_t vm_allocate_early(size_t virtualSize, size_t physicalSize, uint32_t attributes, uintptr_t alignment) __hidden;
int vm_get_page_mapping(pid_t team, uintptr_t vaddr, vm_paddr_t * paddr);

void vm_init_mappings(void) __hidden;
void vm_init(void) __hidden;

void arch_vm_unset_memory_type(struct VMArea *area);
int arch_vm_set_memory_type(struct VMArea *area, vm_paddr_t physicalBase, uint32_t type);

struct kernel_args;
void arch_vm_init_post_area(struct kernel_args *args);
void arch_vm_init_end(struct kernel_args *args);
void arch_vm_init_post_modules(struct kernel_args *args);

#define vm_swap_address_space(from, to) arch_vm_aspace_swap(from, to)
void arch_vm_aspace_swap(struct VMAddressSpace *from,
	struct VMAddressSpace *to);

#if defined(CONFIG_KDEBUGGER)
int vm_debug_copy_page_memory(pid_t teamID, void *unsafeMemory, void *buffer,
							  size_t size, bool copyToUnsafe);
#endif

__END_DECLS

#ifdef __cplusplus
/*
 * Utility class to allow locking
 */

struct scoped_memory_locker {
	constexpr scoped_memory_locker() noexcept = default;

	template<typename U> scoped_memory_locker(U *address, size_t numBytes,
			bool writable,
			std::enable_if_t<!std::is_const_v<U>, int> = 0) noexcept :
			fPID(getpid()), fAddress(reinterpret_cast<void*>(address)), fSize(
					numBytes), fWritable(writable) {
		fError = lock_memory_etc(B_CURRENT_TEAM, fAddress, fSize, writable);

		if (fError == 0) {
			fLocked = true;
		}
	}

	template<typename U> scoped_memory_locker(pid_t pid, U *address,
			size_t numBytes, bool writable,
			std::enable_if_t<!std::is_const_v<U>, int> = 0) noexcept :
			fPID(pid), fAddress(reinterpret_cast<void*>(address)), fSize(
					numBytes), fWritable(writable) {
		fError = lock_memory_etc(fPID, fAddress, fSize, writable);

		if (fError == 0) {
			fLocked = true;
		}
	}

	template<typename U> scoped_memory_locker(const U *address,
			size_t numBytes) noexcept :
			fPID(getpid()), fAddress(
					const_cast<void*>(reinterpret_cast<const void*>(address))), fSize(
					numBytes), fWritable(false) {
		fError = lock_memory_etc(B_CURRENT_TEAM, fAddress, fSize, false);

		if (fError == 0) {
			fLocked = true;
		}
	}

	template<typename U> scoped_memory_locker(pid_t pid, const U *address,
			size_t numBytes) noexcept :
			fPID(pid), fAddress(
					const_cast<void*>(reinterpret_cast<const void*>(address))), fSize(
					numBytes), fWritable(false) {
		fError = lock_memory_etc(fPID, fAddress, fSize, false);

		if (fError == 0) {
			fLocked = true;
		}
	}

	~scoped_memory_locker() noexcept {
		if (fLocked) {
			unlock_memory_etc(fPID, fAddress, fSize, fWritable);
		}
	}

	constexpr scoped_memory_locker(scoped_memory_locker &&other) noexcept :
			fPID(other.fPID), fAddress(other.fAddress), fSize(other.fSize), fWritable(
					other.fWritable), fError(other.fError), fLocked(
					other.fLocked) {
		other.fPID = -1;
		other.fLocked = false;
		other.fSize = 0;
		other.fAddress = nullptr;
		other.fWritable = false;
		other.fError = -ENXIO;
	}

	scoped_memory_locker& operator =(scoped_memory_locker &&other) noexcept {
		if (this != &other) {
			if (fLocked) {
				unlock_memory_etc(fPID, fAddress, fSize, fWritable);
			}
			fPID = other.fPID;
			fAddress = other.fAddress;
			fSize = other.fSize;
			fWritable = other.fWritable;
			fError = other.fError;
			fLocked = other.fLocked;
			other.fPID = -ENXIO;
			other.fLocked = false;
			other.fSize = 0;
			other.fAddress = nullptr;
			other.fWritable = false;
			other.fError = -1;
		}
		return *this;
	}

	[[nodiscard]] int Lock() noexcept {
		if (!fLocked) {
			fError = lock_memory_etc(fPID, fAddress, fSize, fWritable);
			fLocked = (fError == 0);
		}
		return fError;
	}

	void Unlock() noexcept {
		if (fLocked) {
			unlock_memory_etc(fPID, fAddress, fSize, fWritable);
			fLocked = false;
			fError = -ENXIO;
		}
	}

	[[nodiscard]] constexpr bool IsLocked() const noexcept {
		return fLocked;
	}

	[[nodiscard]] constexpr int Error() const noexcept {
		return fError;
	}

	scoped_memory_locker(const scoped_memory_locker&) = delete;
	scoped_memory_locker& operator =(const scoped_memory_locker&) = delete;
private:
	pid_t fPID { -1 };
	void *fAddress { nullptr };
	size_t fSize { 0 };
	bool fWritable { false };
	int fError { -ENXIO };
	bool fLocked { false };
};
#endif

#endif /* !__ASSEMBLY__ */
#endif /* !__MM_H__*/
