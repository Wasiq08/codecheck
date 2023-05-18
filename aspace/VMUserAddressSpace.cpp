
#include "VMUserAddressSpace.h"

#include <mm/VMAddressSpace.h>
#include <lib/OpenHashTable.h>
#include <mm/mm_allocator.h>
#include <mm/mm_cache.h>
#include <cinttypes>
#include <core/debug.h>
#include <lib/cxxabi.h>
#include <core/random.h>

//#define TRACE_VM
#ifdef TRACE_VM
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif

/*!	Verifies that an area with the given aligned base and size fits into
	the spot defined by base and limit and checks for overflows.
 */
static inline bool is_valid_spot(uintptr_t base, uintptr_t alignedBase,
		uintptr_t size, uintptr_t limit, uint32_t protection)
{
//#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
//	if(protection & B_NONSECURE_AREA) {
//		if(alignedBase < VM_NONSECURE_BASE)
//			return false;
//	} else {
//		if(alignedBase + size > VM_NONSECURE_BASE)
//			return false;
//	}
//#else
	(void)protection;
//#endif

	return (alignedBase >= base && alignedBase + (size - 1) > alignedBase
			&& alignedBase + (size - 1) <= limit);
}


static inline bool
is_base_address_spec(uint32_t addressSpec)
{
	return addressSpec == B_BASE_ADDRESS
		|| addressSpec == B_RANDOMIZED_BASE_ADDRESS;
}


static inline uintptr_t
align_address(uintptr_t address, size_t alignment)
{
	return roundup2(address, alignment);
}


static inline uintptr_t
align_address(uintptr_t address, size_t alignment, uint32_t addressSpec,
	uintptr_t baseAddress)
{
	if (is_base_address_spec(addressSpec))
		address = std::max(address, baseAddress);
	return align_address(address, alignment);
}


// #pragma mark - VMUserAddressSpace


VMUserAddressSpace::VMUserAddressSpace(pid_t id, uintptr_t base, size_t size)
	:
	VMAddressSpace(id, base, size, "address space")
{
#if defined(CONFIG_DISABLE_ASLR)
	SetRandomizingEnabled(false);
#endif
}


inline VMArea*
VMUserAddressSpace::FirstArea() const
{
	VMUserArea* area = fAreas.LeftMost();
	while (area != NULL && area->id == VM_RESERVED_AREA_ID)
		area = fAreas.Next(area);
	return area;
}


inline VMArea*
VMUserAddressSpace::NextArea(VMArea* _area) const
{
	VMUserArea* area = static_cast<VMUserArea*>(_area);
	area = fAreas.Next(area);
	while (area != NULL && area->id == VM_RESERVED_AREA_ID)
		area = fAreas.Next(area);
	return area;
}


VMArea*
VMUserAddressSpace::CreateArea(const char* name, uint32_t wiring,
	uint32_t protection, uint32_t allocationFlags)
{
	return VMUserArea::Create(this, name, wiring, protection, allocationFlags);
}


void
VMUserAddressSpace::DeleteArea(VMArea* _area, uint32_t allocationFlags)
{
	VMUserArea *area = static_cast<VMUserArea*>(_area);
	object_cache_delete(sUserAreaCache, area, allocationFlags);
}


//! You must hold the address space's read lock.
VMArea*
VMUserAddressSpace::LookupArea(uintptr_t address) const
{
	VMUserArea* area = fAreas.FindClosest(address, true);
	if (area == NULL || area->id == VM_RESERVED_AREA_ID)
		return NULL;

	return area->ContainsAddress(address) ? area : NULL;
}


//! You must hold the address space's read lock.
VMArea*
VMUserAddressSpace::FindClosestArea(uintptr_t address, bool less) const
{
	VMUserArea* area = fAreas.FindClosest(address, less);
	while (area != NULL && area->id == VM_RESERVED_AREA_ID)
		area = fAreas.Next(area);
	return area;
}

/*!	This inserts the area you pass into the address space.
	It will also set the "_address" argument to its base address when
	the call succeeds.
	You need to hold the VMAddressSpace write lock.
*/
int
VMUserAddressSpace::InsertArea(VMArea* _area, size_t size,
	const virtual_address_restrictions* addressRestrictions,
	uint32_t allocationFlags, void** _address)
{
	VMUserArea* area = static_cast<VMUserArea*>(_area);

	uintptr_t searchBase, searchEnd;
	int status;

	switch (addressRestrictions->address_specification) {
		case B_EXACT_ADDRESS:
			searchBase = (uintptr_t)addressRestrictions->address;
			searchEnd = (uintptr_t)addressRestrictions->address + (size - 1);

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			// NS areas are allowed only from top of the
			// address space

			if(area->protection & B_NONSECURE_AREA) {
				if(searchBase < VM_NONSECURE_BASE)
					return -EINVAL;
			} else {
				if(searchEnd >= VM_NONSECURE_BASE)
					return -EINVAL;
			}
#endif

			break;

		case B_BASE_ADDRESS:
		case B_RANDOMIZED_BASE_ADDRESS:
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			if(area->protection & B_NONSECURE_AREA) {
				searchBase = std::max<uintptr_t>((uintptr_t)addressRestrictions->address, VM_NONSECURE_BASE);
				searchEnd = fEndAddress;
			} else {
				searchBase = std::min<uintptr_t>((uintptr_t)addressRestrictions->address, VM_NONSECURE_BASE - PAGE_SIZE);
				searchEnd = VM_NONSECURE_BASE - 1;
			}
#else
			searchBase = (uintptr_t)addressRestrictions->address;
			searchEnd = fEndAddress;
#endif
			break;

		case B_ANY_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
		case B_RANDOMIZED_ANY_ADDRESS:
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			if(area->protection & B_NONSECURE_AREA) {
				searchBase = VM_NONSECURE_BASE;
				searchEnd = fEndAddress;
			} else {
				searchBase = fBase;
				searchEnd = VM_NONSECURE_BASE - 1;
			}
#else
			searchBase = fBase;
			searchEnd = fEndAddress;
#endif
			break;

		default:
			return -EINVAL;
	}

	// TODO: remove this again when vm86 mode is moved into the kernel
	// completely (currently needs a userland address space!)
	if (addressRestrictions->address_specification != B_EXACT_ADDRESS)
		searchBase = std::max(searchBase, (uintptr_t)VM_HEAP_BASE);

	status = _InsertAreaSlot(searchBase, size, searchEnd,
		addressRestrictions->address_specification,
		addressRestrictions->alignment, area, allocationFlags);
	if (status == 0) {
		if (_address != nullptr)
			*_address = (void*)area->Base();
		fFreeSpace -= area->Size();
	}

	return status;
}


//! You must hold the address space's write lock.
void
VMUserAddressSpace::RemoveArea(VMArea* _area, [[maybe_unused]] uint32_t allocationFlags)
{
	VMUserArea* area = static_cast<VMUserArea*>(_area);

	fAreas.Remove(area);

	if (area->id != VM_RESERVED_AREA_ID) {
		IncrementChangeCount();
		fFreeSpace += area->Size();
	}
}


bool
VMUserAddressSpace::CanResizeArea(VMArea* area, size_t newSize)
{
	VMUserArea* next = fAreas.Next(static_cast<VMUserArea*>(area));
	uintptr_t newEnd = area->Base() + (newSize - 1);

	if (next == nullptr)
		return fEndAddress >= newEnd;

	if (next->Base() > newEnd)
		return true;

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	if(!(area->protection & B_NONSECURE_AREA)) {
		if(newEnd >= VM_NONSECURE_BASE)
			return false;
	}
#endif

	// If the area was created inside a reserved area, it can
	// also be resized in that area
	// TODO: if there is free space after the reserved area, it could
	// be used as well...
	return next->id == VM_RESERVED_AREA_ID
		&& (uint64_t)next->cache_offset <= (uint64_t)area->Base()
		&& next->Base() + (next->Size() - 1) >= newEnd;
}


int
VMUserAddressSpace::ResizeArea(VMArea* _area, size_t newSize,
	uint32_t allocationFlags)
{
	VMUserArea* area = static_cast<VMUserArea*>(_area);

	uintptr_t newEnd = area->Base() + (newSize - 1);
	VMUserArea* next = fAreas.Next(area);
	if (next != nullptr && next->Base() <= newEnd) {
		if (next->id != VM_RESERVED_AREA_ID
			|| (uint64_t)next->cache_offset > (uint64_t)area->Base()
			|| next->Base() + (next->Size() - 1) < newEnd) {
			panic("resize situation for area %p has changed although we "
				"should have the address space lock", area);
		}

		// resize reserved area
		uintptr_t offset = area->Base() + newSize - next->Base();
		if (next->Size() <= offset) {
			RemoveArea(next, allocationFlags);
			object_cache_delete(sUserAreaCache, next, allocationFlags);
		} else {
			int error = ShrinkAreaHead(next, next->Size() - offset,
				allocationFlags);
			if (error != 0)
				return error;
		}
	}

	area->SetSize(newSize);
	return 0;
}


int
VMUserAddressSpace::ShrinkAreaHead(VMArea* area, size_t size,
                                   [[maybe_unused]] uint32_t allocationFlags)
{
	size_t oldSize = area->Size();
	if (size == oldSize)
		return 0;

	area->SetBase(area->Base() + oldSize - size);
	area->SetSize(size);
	area->SetCommitBase(std::max(area->Base(), area->CommitBase()));

	return 0;
}


int
VMUserAddressSpace::ShrinkAreaTail(VMArea* area, size_t size,
                                   [[maybe_unused]] uint32_t allocationFlags)
{
	size_t oldSize = area->Size();
	if (size == oldSize)
		return 0;

	area->SetSize(size);
	area->SetCommitBase(std::min(area->Base() + area->Size(), area->CommitBase()));

	return 0;
}


int
VMUserAddressSpace::ReserveAddressRange(size_t size,
	const virtual_address_restrictions* addressRestrictions,
	uint32_t flags, uint32_t allocationFlags, void** _address)
{
	// check to see if this address space has entered DELETE state
	if (fDeleting) {
		// okay, someone is trying to delete this address space now, so we
		// can't insert the area, let's back out
		return -ESRCH;
	}

	VMUserArea* area = VMUserArea::CreateReserved(this, flags, allocationFlags);
	if (area == nullptr)
		return -ENOMEM;

	int status = InsertArea(area, size, addressRestrictions,
		allocationFlags, _address);
	if (status != 0) {
		object_cache_delete(sUserAreaCache, area, allocationFlags);
		return status;
	}

	area->cache_offset = area->Base();
		// we cache the original base address here

	Get();
	return 0;
}


int
VMUserAddressSpace::UnreserveAddressRange(uintptr_t address, size_t size,
	uint32_t allocationFlags)
{
	// check to see if this address space has entered DELETE state
	if (fDeleting) {
		// okay, someone is trying to delete this address space now, so we can't
		// insert the area, so back out
		return -ESRCH;
	}

	// the area must be completely part of the reserved range
	VMUserArea* area = fAreas.FindClosest(address, false);
	if (area == nullptr)
		return 0;

	uintptr_t endAddress = address + size - 1;
	for (VMUserAreaTree::Iterator it = fAreas.GetIterator(area);
		(area = it.Next()) != NULL
			&& area->Base() + area->Size() - 1 <= endAddress;) {

		if (area->id == VM_RESERVED_AREA_ID) {
			// remove reserved range
			RemoveArea(area, allocationFlags);
			Put();
			object_cache_delete(sUserAreaCache, area, allocationFlags);
		}
	}

	return 0;
}


void
VMUserAddressSpace::UnreserveAllAddressRanges(uint32_t allocationFlags)
{
	for (VMUserAreaTree::Iterator it = fAreas.GetIterator();
			VMUserArea* area = it.Next();) {
		if (area->id == VM_RESERVED_AREA_ID) {
			RemoveArea(area, allocationFlags);
			Put();
			object_cache_delete(sUserAreaCache, area, allocationFlags);
		}
	}
}


/*!	Finds a reserved area that covers the region spanned by \a start and
	\a size, inserts the \a area into that region and makes sure that
	there are reserved regions for the remaining parts.
*/
int
VMUserAddressSpace::_InsertAreaIntoReservedRegion(uintptr_t start, size_t size,
	VMUserArea* area, uint32_t allocationFlags)
{
	VMUserArea* reserved = fAreas.FindClosest(start, true);
	if (reserved == NULL
		|| !reserved->ContainsAddress(start)
		|| !reserved->ContainsAddress(start + size - 1)) {
		return -ENOENT;
	}

	// This area covers the requested range
	if (reserved->id != VM_RESERVED_AREA_ID) {
		// but it's not reserved space, it's a real area
		return -EINVAL;
	}

	// Now we have to transfer the requested part of the reserved
	// range to the new area - and remove, resize or split the old
	// reserved area.

	if (start == reserved->Base()) {
		// the area starts at the beginning of the reserved range

		if (size == reserved->Size()) {
			// the new area fully covers the reversed range
			fAreas.Remove(reserved);
			Put();
			object_cache_delete(sUserAreaCache, reserved, allocationFlags);
		} else {
			// resize the reserved range behind the area
			reserved->SetBase(reserved->Base() + size);
			reserved->SetSize(reserved->Size() - size);
		}
	} else if (start + size == reserved->Base() + reserved->Size()) {
		// the area is at the end of the reserved range
		// resize the reserved range before the area
		reserved->SetSize(start - reserved->Base());
	} else {
		// the area splits the reserved range into two separate ones
		// we need a new reserved area to cover this space
		VMUserArea* newReserved = VMUserArea::CreateReserved(this,
				reserved->protection, allocationFlags);
		if (newReserved == nullptr)
			return -ENOMEM;

		Get();

		// resize regions
		newReserved->SetBase(start + size);
		newReserved->SetSize(
			reserved->Base() + reserved->Size() - start - size);
		newReserved->cache_offset = reserved->cache_offset;
		reserved->SetSize(start - reserved->Base());

		fAreas.Insert(newReserved);
	}

	area->SetBase(start);
	area->SetSize(size);
	area->SetCommitBase(start + size);
	fAreas.Insert(area);
	IncrementChangeCount();

	return 0;
}


/*!	Must be called with this address space's write lock held */
int
VMUserAddressSpace::_InsertAreaSlot(uintptr_t start, uintptr_t size, uintptr_t end,
	uint32_t addressSpec, size_t alignment, VMUserArea* area,
	uint32_t allocationFlags)
{
	TRACE(("VMUserAddressSpace::_InsertAreaSlot: address space %p, start "
		"0x%" PRIxPTR ", size %zd, end 0x%" PRIxPTR " , addressSpec %" PRIu32 ", area %p\n",
		this, start, size, end, addressSpec, area));

	// do some sanity checking
	if (start < fBase || size == 0 || start + (size - 1) > end)
		return -EFAULT;
	if (end > fEndAddress)
		return -ENOMEM;

	if (addressSpec == B_EXACT_ADDRESS && area->id != VM_RESERVED_AREA_ID) {
		// search for a reserved area
		int status = _InsertAreaIntoReservedRegion(start, size, area,
			allocationFlags);
		if (status == 0 || status == -EINVAL)
			return status;

		// There was no reserved area, and the slot doesn't seem to be used
		// already
		// TODO: this could be further optimized.
	}

	if (alignment == 0)
		alignment = PAGE_SIZE;
	if (addressSpec == B_ANY_KERNEL_BLOCK_ADDRESS) {
		size_t maximumAnyAlignment = TranslationMap()->HardwareBlockSize();

		// align the memory to the next power of two of the size
		while (alignment < size && alignment < maximumAnyAlignment)
			alignment <<= 1;
	}

	start = align_address(start, alignment);

	bool useHint
		= addressSpec != B_EXACT_ADDRESS && !is_base_address_spec(addressSpec);

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	if(area->protection & B_NONSECURE_AREA) {
		useHint = false;
	}
#endif

	uintptr_t originalStart = 0;
	if (fRandomizingEnabled && addressSpec == B_RANDOMIZED_BASE_ADDRESS) {
		originalStart = start;
		start = _RandomizeAddress(start, end - size + 1, alignment, true);
	} else if (useHint
		&& start <= fNextInsertHint && fNextInsertHint <= end - size + 1) {
		originalStart = start;
		start = fNextInsertHint;
	}

	// walk up to the spot where we should start searching
second_chance:
	VMUserArea* next = fAreas.FindClosest(start + size, false);
	VMUserArea* last = next != NULL
			? fAreas.Previous(next) : fAreas.FindClosest(start + size, true);

	// find the right spot depending on the address specification - the area
	// will be inserted directly after "last" ("next" is not referenced anymore)

	bool foundSpot = false;
	switch (addressSpec) {
		case B_ANY_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
		case B_RANDOMIZED_ANY_ADDRESS:
		case B_BASE_ADDRESS:
		case B_RANDOMIZED_BASE_ADDRESS:
		{
			VMUserAreaTree::Iterator it = fAreas.GetIterator(
				next != NULL ? next : fAreas.LeftMost());

			// find a hole big enough for a new area
			if (last == nullptr) {
				// see if we can build it at the beginning of the virtual map
				uintptr_t alignedBase = align_address(start, alignment);
				uintptr_t nextBase = next == nullptr
					? end : std::min(next->Base() - 1, end);
				if (is_valid_spot(start, alignedBase, size, nextBase, area->protection)) {
					uintptr_t rangeEnd = std::min(nextBase - size + 1, end);
					if (_IsRandomized(addressSpec)) {
						alignedBase = _RandomizeAddress(alignedBase, rangeEnd,
							alignment);
					}

					foundSpot = true;
					area->SetBase(alignedBase);
					break;
				}

				last = next;
				next = it.Next();
			}

			// keep walking
			while (next != nullptr && next->Base() + next->Size() - 1 <= end) {
				uintptr_t alignedBase = align_address(last->Base() + last->Size(),
					alignment, addressSpec, start);
				uintptr_t nextBase = std::min(end, next->Base() - 1);

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
				if(area->protection & B_NONSECURE_AREA) {
					alignedBase = std::max<uintptr_t>(alignedBase, VM_NONSECURE_BASE);
				}
#endif

				if (is_valid_spot(last->Base() + (last->Size() - 1),
						alignedBase, size, nextBase, area->protection)) {
					uintptr_t rangeEnd = std::min(nextBase - size + 1, end);
					if (_IsRandomized(addressSpec)) {
						alignedBase = _RandomizeAddress(alignedBase,
							rangeEnd, alignment);
					}

					foundSpot = true;
					area->SetBase(alignedBase);
					break;
				}

				last = next;
				next = it.Next();
			}

			if (foundSpot)
				break;

			uintptr_t alignedBase = align_address(last->Base() + last->Size(),
				alignment, addressSpec, start);

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			if(area->protection & B_NONSECURE_AREA) {
				alignedBase = std::max<uintptr_t>(alignedBase, VM_NONSECURE_BASE);
			}
#endif

			if (next == nullptr && is_valid_spot(last->Base() + (last->Size() - 1),
					alignedBase, size, end, area->protection)) {
				if (_IsRandomized(addressSpec)) {
					alignedBase = _RandomizeAddress(alignedBase, end - size + 1,
						alignment);
				}

				// got a spot
				foundSpot = true;
				area->SetBase(alignedBase);
				break;
			} else if (is_base_address_spec(addressSpec)
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
					|| (area->protection & B_NONSECURE_AREA) // Never to "any" search in case of NS here
#endif
					) {
				// we didn't find a free spot in the requested range, so we'll
				// try again without any restrictions
				if (!_IsRandomized(addressSpec)) {
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
					if(area->protection & B_NONSECURE_AREA)
						start = VM_NONSECURE_BASE;
					else
#endif
						start = VM_HEAP_BASE;
					addressSpec = B_ANY_ADDRESS;
				} else if (start == originalStart) {
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
					if(area->protection & B_NONSECURE_AREA)
						start = VM_NONSECURE_BASE;
					else
#endif
						start = VM_HEAP_BASE;
					addressSpec = B_RANDOMIZED_ANY_ADDRESS;
				} else {
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
					if(area->protection & B_NONSECURE_AREA)
						start = VM_NONSECURE_BASE;
					else
#endif
						start = originalStart;
					addressSpec = B_RANDOMIZED_BASE_ADDRESS;
				}

				goto second_chance;
			} else if (useHint
					&& originalStart != 0 && start != originalStart) {
				start = originalStart;
				goto second_chance;
			} else if (area->id != VM_RESERVED_AREA_ID) {
				// We didn't find a free spot - if there are any reserved areas,
				// we can now test those for free space
				// TODO: it would make sense to start with the biggest of them
				it = fAreas.GetIterator();
				next = it.Next();
				for (last = nullptr; next != nullptr; next = it.Next()) {
					if (next->id != VM_RESERVED_AREA_ID) {
						last = next;
						continue;
					} else if (next->Base() + size - 1 > end)
						break;

					// TODO: take free space after the reserved area into
					// account!
					uintptr_t newAlignedBase = align_address(next->Base(), alignment);
					if (next->Base() == newAlignedBase && next->Size() == size) {
						// The reserved area is entirely covered, and thus,
						// removed
						fAreas.Remove(next);

						foundSpot = true;
						area->SetBase(newAlignedBase);
						object_cache_delete(sUserAreaCache, next, allocationFlags);
						break;
					}

					if ((next->protection & RESERVED_AVOID_BASE) == 0
						&& newAlignedBase == next->Base()
						&& next->Size() >= size) {
						uintptr_t rangeEnd = std::min(
							next->Base() + next->Size() - size, end);
						if (_IsRandomized(addressSpec)) {
							newAlignedBase = _RandomizeAddress(next->Base(),
								rangeEnd, alignment);
						}
						uintptr_t offset = newAlignedBase - next->Base();

						// The new area will be placed at the beginning of the
						// reserved area and the reserved area will be offset
						// and resized
						foundSpot = true;
						next->SetBase(next->Base() + offset + size);
						next->SetSize(next->Size() - offset - size);
						area->SetBase(newAlignedBase);
						break;
					}

					if (is_valid_spot(next->Base(), newAlignedBase, size,
							std::min(next->Base() + next->Size() - 1, end), area->protection)) {
						// The new area will be placed at the end of the
						// reserved area, and the reserved area will be resized
						// to make space

						if (_IsRandomized(addressSpec)) {
							uintptr_t alignedNextBase = align_address(next->Base(),
								alignment);

							uintptr_t startRange = next->Base() + next->Size();
							startRange -= size + max_randomize();
							startRange = rounddown2(startRange, alignment);
							startRange = std::max(startRange, alignedNextBase);

							uintptr_t rangeEnd
								= std::min(next->Base() + next->Size() - size,
									end);
							newAlignedBase = _RandomizeAddress(startRange,
								rangeEnd, alignment);
						} else {
							newAlignedBase = rounddown2(
								next->Base() + next->Size() - size, alignment);
						}

						foundSpot = true;
						next->SetSize(newAlignedBase - next->Base());
						area->SetBase(newAlignedBase);
						break;
					}

					last = next;
				}
			}

			break;
		}

		case B_EXACT_ADDRESS:
			// see if we can create it exactly here
			if ((last == nullptr || last->Base() + (last->Size() - 1) < start)
				&& (next == nullptr || next->Base() > start + (size - 1))) {
				foundSpot = true;
				area->SetBase(start);
				break;
			}
			break;
		default:
			return -EINVAL;
	}

	if (!foundSpot)
		return addressSpec == B_EXACT_ADDRESS ? -EINVAL : -ENOMEM;

	if (useHint)
		fNextInsertHint = area->Base() + size;

	area->SetSize(size);
	area->SetCommitBase(area->Base() + size);

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	if (!(area->protection & B_NONSECURE_AREA)) {
		if (area->End() > VM_NONSECURE_BASE) {
			printk(KERN_ERR|WITHUART,
					"Bug in VM search ? S area %s insside NS reservation @ %" PRIxPTR "\n",
					area->name, area->Base());
			return -ENOMEM;
		}
	} else {
		if (area->Base() < VM_NONSECURE_BASE) {
			printk(KERN_ERR|WITHUART,
					"Bug in VM search ? NS area %s outside NS reservation @ %" PRIxPTR "\n",
					area->name, area->Base());
			return -ENOMEM;
		}
	}
#endif

	fAreas.Insert(area);
	IncrementChangeCount();
	return 0;
}

void
VMUserAddressSpace::Dump()
{
#if defined(CONFIG_KDEBUGGER)
	VMAddressSpace::Dump();
	kprintf("area_list:\n");

	for (auto it = fAreas.GetIterator();
			auto* area = it.Next();) {
		kprintf(" area 0x%" PRIx32 ": ", area->id);
		kprintf("base_addr = 0x%" PRIxPTR " ", area->Base());
		kprintf("size = 0x%zx ", area->Size());
		kprintf("name = '%s' ", area->name);
		kprintf("protection = 0x%" PRIx32 "\n", area->protection);
	}
#endif
}
