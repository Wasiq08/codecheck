
#ifndef VM_USER_ADDRESS_SPACE_H
#define VM_USER_ADDRESS_SPACE_H


#include <mm/VMAddressSpace.h>

#include "VMUserArea.h"


struct VMUserAddressSpace final : VMAddressSpace {
public:
							VMUserAddressSpace(pid_t id, uintptr_t base,
									size_t size);

		VMArea*				FirstArea() const override;
		VMArea*				NextArea(VMArea* area) const override;

		[[nodiscard]] VMArea*LookupArea(uintptr_t address) const override;
		[[nodiscard]] VMArea*FindClosestArea(uintptr_t address, bool less) const override;
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
			int     			_InsertAreaIntoReservedRegion(uintptr_t start,
									size_t size, VMUserArea* area,
									uint32_t allocationFlags);
			int     			_InsertAreaSlot(uintptr_t start, uintptr_t size,
									uintptr_t end, uint32_t addressSpec,
									size_t alignment, VMUserArea* area,
									uint32_t allocationFlags);

private:
			VMUserAreaTree		fAreas;
			uintptr_t			fNextInsertHint = 0;
};


#endif	/* VM_USER_ADDRESS_SPACE_H */
