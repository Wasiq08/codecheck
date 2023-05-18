
#include "VMKernelArea.h"

#include <mm/mm_allocator.h>


VMKernelArea::VMKernelArea(VMAddressSpace* addressSpace, uint32_t areaWiring,
	uint32_t areaProtection)
	:
	VMArea(addressSpace, areaWiring, areaProtection)
{
}

/*static*/ VMKernelArea*
VMKernelArea::Create(VMAddressSpace* addressSpace, const char* name,
	uint32_t wiring, uint32_t protection, ObjectCache* objectCache,
	uint32_t allocationFlags)
{
	VMKernelArea* area = new(objectCache, allocationFlags) VMKernelArea(
		addressSpace, wiring, protection);

	if (area) {
		area->Init(name);
	}

	return area;
}
