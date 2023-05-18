

#include "VMUserArea.h"
#include "VMAddressSpace.h"

#include <mm/mm_allocator.h>
#include <mm/xmalloc.h>
#include <lib/cxxabi.h>

VMUserArea::VMUserArea(VMAddressSpace* addressSpace, uint32_t areaWiring,
	uint32_t areaProtection)
	:
	VMArea(addressSpace, areaWiring, areaProtection)
{
}


/*static*/ VMUserArea*
VMUserArea::Create(VMAddressSpace* addressSpace, const char* name,
	uint32_t wiring, uint32_t protection, uint32_t allocationFlags)
{
	VMUserArea* area = new(VMAddressSpace::GetUserAreaCache(), allocationFlags) VMUserArea(
		addressSpace, wiring, protection);
	if (area) {
		area->Init(name);
	}
	return area;
}


/*static*/ VMUserArea*
VMUserArea::CreateReserved(VMAddressSpace* addressSpace, uint32_t flags,
	uint32_t allocationFlags)
{
	VMUserArea *area =
			new (VMAddressSpace::GetUserAreaCache(), allocationFlags) VMUserArea(
					addressSpace, 0, 0);
	if (area != nullptr) {
		area->id = VM_RESERVED_AREA_ID;
		area->protection = flags;
	}
	return area;
}
