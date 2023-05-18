
#ifndef VM_KERNEL_ADDRESS_SPACE_H
#define VM_KERNEL_ADDRESS_SPACE_H

#include <core/kconfig.h>
#include <mm/VMAddressSpace.h>

#include "VMKernelArea.h"


struct ObjectCache;


struct VMKernelAddressSpace final : VMAddressSpace {
public:
								VMKernelAddressSpace(pid_t id, uintptr_t base,
									size_t size);
							~VMKernelAddressSpace() override;

		int     			InitObject() override;

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			void				InitNonSecure();
#endif

	[[nodiscard]] 	VMArea*				FirstArea() const override;
		VMArea*				NextArea(VMArea* area) const override;

	[[nodiscard]] 	VMArea*				LookupArea(uintptr_t address) const override;
	[[nodiscard]] 	VMArea*				FindClosestArea(uintptr_t address, bool less) const	override;
		VMArea*				CreateArea(const char* name, uint32_t wiring,
									uint32_t protection, uint32_t allocationFlags) override;
		void				DeleteArea(VMArea* area,
									uint32_t allocationFlags) override;
		int     			InsertArea(VMArea* area, size_t size,
									const virtual_address_restrictions*
										addressRestrictions,
									uint32_t allocationFlags, void** _address) override;
		void				RemoveArea(VMArea* area,
									uint32_t allocationFlags) override;

		bool				CanResizeArea(VMArea* area, size_t newSize) override;
		int     			ResizeArea(VMArea* area, size_t newSize,
									uint32_t allocationFlags) override;
		int     			ShrinkAreaHead(VMArea* area, size_t newSize,
									uint32_t allocationFlags) override;
		int     			ShrinkAreaTail(VMArea* area, size_t newSize,
									uint32_t allocationFlags) override;

		int     			ReserveAddressRange(size_t size,
									const virtual_address_restrictions*
										addressRestrictions,
									uint32_t flags, uint32_t allocationFlags,
									void** _address) override;
		int     			UnreserveAddressRange(uintptr_t address,
									size_t size, uint32_t allocationFlags) override;
		void				UnreserveAllAddressRanges(
									uint32_t allocationFlags) override;

		void				Dump() override;

private:
			typedef VMKernelAddressRange Range;
			using RangeTree = VMKernelAddressRangeTree;
			using RangeList = DoublyLinkedList<Range, DoublyLinkedListMemberGetLink<Range, &Range::listLink> >;
			using RangeFreeList = DoublyLinkedList<Range, VMKernelAddressRangeGetFreeListLink>;

private:
	inline	void				_FreeListInsertRange(Range* range, size_t size);
	inline	void				_FreeListRemoveRange(Range* range, size_t size);

			void				_InsertRange(Range* range);
			void				_RemoveRange(Range* range);

			int     			_AllocateRange(
									const virtual_address_restrictions*
										addressRestrictions,
									size_t size, bool allowReservedRange,
									uint32_t allocationFlags, Range*& _range
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
									, bool nonSecue
#endif
									);
			Range*				_FindFreeRange(uintptr_t start, size_t size,
									size_t alignment, uint32_t addressSpec,
									bool allowReservedRange,
									uintptr_t& _foundAddress
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
									, bool nonSecure
#endif
									);
			void				_FreeRange(Range* range,
									uint32_t allocationFlags);

			void				_CheckStructures() const;

private:
			RangeTree			fRangeTree;
			RangeList			fRangeList;
			RangeFreeList*		fFreeLists;
			int					fFreeListCount;
			ObjectCache*		fAreaObjectCache;
			ObjectCache*		fRangesObjectCache;

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			uintptr_t			fNSRangeBase;
			uintptr_t			fNSRangeEnd;
#endif
};


#endif	/* VM_KERNEL_ADDRESS_SPACE_H */
