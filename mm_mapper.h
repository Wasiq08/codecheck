#ifndef _MM_MM_MAPPER_H_
#define _MM_MM_MAPPER_H_

#include <core/mutex.h>
#include <mm/page.h>
#include <sys/param.h>
#include <lib/AutoLock.h>

struct ObjectCache;
struct VMArea;

#define PAGE_MODIFIED			0x10000000
#define PAGE_ACCESSED			0x20000000
#define PAGE_PRESENT			0x80000000

#ifdef __cplusplus
struct VMTranslationMap {
	struct ReverseMappingInfoCallback;

	virtual ~VMTranslationMap() = default;

	virtual bool Lock() = 0;
	virtual void Unlock() = 0;

	/*!
	 * Return size of hardware block for large pages. This is minimum
	 * preferred alignment in case physical address and large mappings
	 * are used
	 *
	 * @return Required huge page TLB alignment
	 */
	[[nodiscard]] virtual size_t HardwareBlockSize() const = 0;

	/*!
	 * Determine whether Accessed bit in the TLB is managed by hardware
	 * or software level. Hardware managed Accessed bit requires different
	 * treatment of pages by the page daemon. In case of S/W managed Access bit,
	 * page daemon will never remove it on pages which are wired, as this would
	 * result in kernel taking page faults in unpredicatble contexts.
	 *
	 * @return true if Accessed bit is managed by software
	 */
	[[nodiscard]] virtual bool VirtualAccessedBit() const { return false; }

	[[nodiscard]] virtual uintptr_t MappedSize() const = 0;
	[[nodiscard]] virtual size_t MaxPagesNeededToMap(uintptr_t start, size_t size) const = 0;

	virtual int Map(uintptr_t virtualAddress, vm_paddr_t physicalAddress,
			uint32_t attributes, vm_reservation_t* reservation) = 0;

	virtual int MapPhysical(uintptr_t virtualAddress,
			vm_paddr_t physicalAddress, size_t size, uint32_t attributes,
			vm_reservation_t* reservation) = 0;

	virtual int Unmap(uintptr_t start, size_t size) = 0;

	virtual int DebugMarkRangePresent(uintptr_t start, uintptr_t end,
			bool markPresent);

	// map not locked
	virtual int UnmapPage(VMArea* area, uintptr_t address,
			bool updatePageQueue) = 0;
	virtual void UnmapPages(VMArea* area, uintptr_t base, size_t size,
			bool updatePageQueue);
	virtual void UnmapArea(VMArea* area, bool deletingAddressSpace,
			bool ignoreTopCachePageFlags);

	virtual int Query(uintptr_t virtualAddress, vm_paddr_t* _physicalAddress,
			uint32_t* _flags) = 0;
	virtual int QueryInterrupt(uintptr_t virtualAddress,
			vm_paddr_t* _physicalAddress, uint32_t* _flags) = 0;

	virtual int Protect(uintptr_t base, size_t size, uint32_t attributes) = 0;

	int ProtectPage(VMArea* area, uintptr_t address, uint32_t attributes);
	int ProtectArea(VMArea* area, uint32_t attributes);

	virtual int ClearFlags(uintptr_t virtualAddress, uint32_t flags) = 0;

	virtual bool ClearAccessedAndModified(VMArea* area, uintptr_t address,
			bool unmapIfUnaccessed, bool& _modified) = 0;

	/*!
	 * Flush TLB for the entire translation map.
	 */
	virtual void Flush() = 0;

	// backends for KDL commands
	virtual void DebugPrintMappingInfo(uintptr_t virtualAddress);
	virtual bool DebugGetReverseMappingInfo(vm_paddr_t physicalAddress,
			ReverseMappingInfoCallback& callback);

	virtual void DumpBadAbort(uintptr_t address);

	/*!
	 * @return Process ID of translation map. This can be used by architecture
	 * specific ports
	 */
	u_long MapId() const noexcept {
		return fMapId;
	}

	/*!
	 * Set translation map ID (PID of process)
	 *
	 * @param id PID of translation map
	 */
	void SetMapId(u_long id) noexcept {
		fMapId = id;
	}
protected:
	void PageUnmapped(VMArea* area, vm_pindex_t pageNumber, bool accessed,
			bool modified, bool updatePageQueue, RecursiveLocker& locker);
	void UnaccessedPageUnmapped(VMArea* area, vm_pindex_t pageNumber,
			RecursiveLocker& locker);

protected:
	recursive_mutex_t fLock{"VMTranslationMap"};
	int32_t fMapCount = 0;
	u_long fMapId = 0;
};

struct VMTranslationMap::ReverseMappingInfoCallback {
	virtual ~ReverseMappingInfoCallback() = default;

	virtual bool HandleVirtualAddress(uintptr_t virtualAddress) = 0;
};

struct VMPhysicalPageMapper {
	virtual ~VMPhysicalPageMapper() = default;

	// get/put virtual address for physical page -- will be usuable on all CPUs
	// (usually more expensive than the *_current_cpu() versions)
	virtual int GetPage(vm_paddr_t physicalAddress, uintptr_t* _virtualAddress,
			void** _handle) = 0;
	virtual int PutPage(uintptr_t virtualAddress, void* handle) = 0;

	// get/put virtual address for physical page -- thread must be pinned the
	// whole time
	virtual int GetPageCurrentCPU(vm_paddr_t physicalAddress,
			uintptr_t* _virtualAddress, void** _handle) = 0;
	virtual int PutPageCurrentCPU(uintptr_t virtualAddress, void* _handle) = 0;

	// get/put virtual address for physical in KDL
	virtual int GetPageDebug(vm_paddr_t physicalAddress,
			uintptr_t* _virtualAddress, void** _handle) = 0;
	virtual int PutPageDebug(uintptr_t virtualAddress, void* handle) = 0;

	// memory operations on pages
	virtual int MemsetPhysical(vm_paddr_t address, int value,
			vm_size_t length) = 0;
	virtual int MemcpyFromPhysical(void* to, vm_paddr_t from, size_t length,
			bool user) = 0;
	virtual int MemcpyToPhysical(vm_paddr_t to, const void* from, size_t length,
			bool user) = 0;
	virtual void MemcpyPhysicalPage(vm_paddr_t to, vm_paddr_t from) = 0;
};

extern ObjectCache * gPageMappingsObjectCache;
#endif

__BEGIN_DECLS

int arch_vm_translation_map_create_map(bool kernel, struct VMTranslationMap** _map);
int arch_vm_translation_map_init(struct VMPhysicalPageMapper** _physicalPageMapper);
int arch_vm_translation_map_init_post_area();
int arch_vm_translation_map_init_post_sem();
int arch_vm_translation_map_early_map(uintptr_t va, vm_paddr_t pa, uint32_t attributes, vm_paddr_t (*get_free_page)());
bool arch_vm_translation_map_is_kernel_page_accessible(uintptr_t virtualAddress, uint32_t protection);
void arch_vm_translation_map_activate(struct VMTranslationMap * map);

__END_DECLS

#endif /* _MM_MM_MAPPER_H_ */
