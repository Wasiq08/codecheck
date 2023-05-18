
#include <core/task.h>
#include <core/debug.h>
#include <core/error.h>
#include <mm/mm.h>
#include <mm/mm_mapper.h>
#include <mm/mm_allocator.h>
#include <mm/mm_cache.h>
#include <mm/VMAddressSpace.h>
#include <trustware/vm.h>

int VMTranslationMap::DebugMarkRangePresent(uintptr_t, uintptr_t,
			bool)
{
	return -EOPNOTSUPP;
}

int VMTranslationMap::ProtectPage(VMArea* area, uintptr_t address,
		uint32_t attributes) {
	return Protect(address, PAGE_SIZE,
			(attributes & ~B_KERNEL_MEMORY_TYPE_MASK)
					| (area->protection & B_KERNEL_MEMORY_TYPE_MASK));
}

int VMTranslationMap::ProtectArea(VMArea* area, uint32_t attributes) {
	return Protect(area->Base(), area->Size(),
			(attributes & ~B_KERNEL_MEMORY_TYPE_MASK)
					| (area->protection & B_KERNEL_MEMORY_TYPE_MASK));
}

void VMTranslationMap::UnmapPages(VMArea * area, uintptr_t base, size_t size, bool updatePageQueue)
{
	ASSERT(base % PAGE_SIZE == 0);
	ASSERT(size % PAGE_SIZE == 0);

	uintptr_t address = base;
	uintptr_t end = address + size;

	for (; address != end; address += PAGE_SIZE)
		UnmapPage(area, address, updatePageQueue);
}

void VMTranslationMap::UnmapArea(VMArea* area, bool, bool)
{
	uintptr_t address = area->Base();
	uintptr_t end = area->Base() + area->Size();

	for (; address != end; address += PAGE_SIZE)
		UnmapPage(area, address, true);
}

void VMTranslationMap::DebugPrintMappingInfo(uintptr_t)
{
}

bool
VMTranslationMap::DebugGetReverseMappingInfo(vm_paddr_t,
	ReverseMappingInfoCallback&)
{
	return false;
}

void VMTranslationMap::PageUnmapped(VMArea* area, vm_pindex_t pageNumber,
	bool accessed, bool modified, bool updatePageQueue, RecursiveLocker& locker)
{
	if (area->cache_type >= CACHE_TYPE_DEVICE) {
		return;
	}

	// get the page
	struct page * page = pfn_to_page(pageNumber);

	if(!page) {
		panic("PageUnmapped: Invalid page %#" B_PRIxPHYSADDR ", accessed: %d, modified: %d", (vm_paddr_t)pageNumber, accessed, modified);
	}

	// transfer the accessed/dirty flags to the page
	if(accessed)
		page->accessed = 1;
	if(modified)
		page->modified = 1;

	// remove the mapping object/decrement the wired_count of the page
	vm_page_mapping* mapping = nullptr;
	if (area->wiring == B_NO_LOCK) {
		vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
		while ((mapping = iterator.Next()) != nullptr) {
			if (mapping->area == area) {
				area->mappings.Remove(mapping);
				page->mappings.Remove(mapping);
				break;
			}
		}
		if (!mapping) {
			panic(
					"PageUnmapped: Can't find mapping for page %#" B_PRIxPHYSADDR ", accessed: %d, modified: %d in area %p",
					(vm_paddr_t) pageNumber, accessed, modified, area);
		}
	} else {
		page->DecrementWiredCount();
	}

	locker.Unlock();

	if (!page->IsMapped()) {
		__atomic_fetch_sub(&gMappedPagesCount, 1, __ATOMIC_RELAXED);

		if (updatePageQueue) {
			if (page->Cache()->temporary)
				vm_page_set_state(page, PAGE_STATE_INACTIVE);
			else if (page->modified)
				vm_page_set_state(page, PAGE_STATE_MODIFIED);
			else
				vm_page_set_state(page, PAGE_STATE_CACHED);
		}
	}

	if (mapping != nullptr) {
		object_cache_free(gPageMappingsObjectCache, mapping, area->address_space == VMAddressSpace::Kernel() ? CACHE_DONT_LOCK_KERNEL : 0);
	}
}

void VMTranslationMap::UnaccessedPageUnmapped(VMArea * area, vm_pindex_t pageNumber, RecursiveLocker& locker)
{
	if (area->cache_type >= CACHE_TYPE_DEVICE) {
		return;
	}

	// get the page
	struct page * page = pfn_to_page(pageNumber);

	if(!page) {
		panic("UnaccessedPageUnmapped: Invalid page %#" B_PRIxPHYSADDR, (vm_paddr_t)pageNumber);
	}

	// remove the mapping object/decrement the wired_count of the page
	vm_page_mapping* mapping = nullptr;
	if (area->wiring == B_NO_LOCK) {
		vm_page_mappings::Iterator iterator = page->mappings.GetIterator();
		while ((mapping = iterator.Next()) != nullptr) {
			if (mapping->area == area) {
				area->mappings.Remove(mapping);
				page->mappings.Remove(mapping);
				break;
			}
		}

		if (!mapping) {
			panic("UnaccessedPageUnmapped: Invalid page %#" B_PRIxPHYSADDR " for area %p", (vm_paddr_t)pageNumber, area);
		}
	} else {
		page->DecrementWiredCount();
	}

	locker.Unlock();

	if (!page->IsMapped()) {
		__atomic_fetch_sub(&gMappedPagesCount, 1, __ATOMIC_RELAXED);
	}

	if (mapping != nullptr) {
		object_cache_free(gPageMappingsObjectCache, mapping, area->address_space == VMAddressSpace::Kernel() ? CACHE_DONT_LOCK_KERNEL : 0);
	}
}

void VMTranslationMap::DumpBadAbort(uintptr_t) {

}
