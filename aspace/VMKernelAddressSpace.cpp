
#include "VMKernelAddressSpace.h"

#include <mm/VMAddressSpace.h>
#include <lib/OpenHashTable.h>
#include <mm/mm_allocator.h>
#include <mm/mm_cache.h>
#include <cinttypes>
#include <core/debug.h>
#include <core/random.h>
#include <lib/cxxabi.h>

//#define TRACE_VM
#ifdef TRACE_VM
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


//#define PARANOIA_CHECKS
#ifdef PARANOIA_CHECKS
#	define PARANOIA_CHECK_STRUCTURES()	_CheckStructures()
#else
#	define PARANOIA_CHECK_STRUCTURES()	do {} while (false)
#endif


static int
ld(size_t value)
{
	int index = -1;
	while (value > 0) {
		value >>= 1;
		index++;
	}

	return index;
}


/*!	Verifies that an area with the given aligned base and size fits into
	the spot defined by base and limit and checks for overflows.
	\param base Minimum base address valid for the area.
	\param alignedBase The base address of the range to check.
	\param size The size of the area.
	\param limit The last (inclusive) addresss of the range to check.
*/
static inline bool
is_valid_spot(uintptr_t base, uintptr_t alignedBase, uintptr_t size, uintptr_t limit)
{
	return (alignedBase >= base && alignedBase + (size - 1) > alignedBase
		&& alignedBase + (size - 1) <= limit);
}


// #pragma mark - VMKernelAddressSpace


VMKernelAddressSpace::VMKernelAddressSpace(pid_t id, uintptr_t base, size_t size)
	:
	VMAddressSpace(id, base, size, "kernel address space"),
	fFreeLists(nullptr),
	fFreeListCount(0),
	fAreaObjectCache(nullptr),
	fRangesObjectCache(nullptr)
{
#if defined(CONFIG_DISABLE_KASLR)
	SetRandomizingEnabled(false);
#endif
}


VMKernelAddressSpace::~VMKernelAddressSpace()
{
	panic("deleting the kernel aspace!\n");
}


int
VMKernelAddressSpace::InitObject()
{
	fAreaObjectCache = create_object_cache("kernel areas",
		sizeof(VMKernelArea), 0, nullptr, nullptr, nullptr);
	if (fAreaObjectCache == nullptr)
		return -ENOMEM;

	fRangesObjectCache = create_object_cache("kernel address ranges",
		sizeof(Range), 0, nullptr, nullptr, nullptr);
	if (fRangesObjectCache == nullptr)
		return -ENOMEM;

	// create the free lists
	size_t size = fEndAddress - fBase + 1;
	fFreeListCount = ld(size) - PAGE_SHIFT + 1;
	fFreeLists = new(std::nothrow) RangeFreeList[fFreeListCount];
	if (fFreeLists == nullptr)
		return -ENOMEM;

	Range* range = new(fRangesObjectCache, 0) Range(fBase, size,
		Range::RANGE_FREE);
	if (range == nullptr)
		return -ENOMEM;

	_InsertRange(range);

	TRACE("VMKernelAddressSpace::InitObject(): address range: %#" PRIxPTR
		" - %#" PRIxPTR ", free lists: %d\n", fBase, fEndAddress,
		fFreeListCount);

	return 0;
}

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
void
VMKernelAddressSpace::InitNonSecure()
{
	size_t nsSize = 0x20000000;
	void *base = nullptr;

	virtual_address_restrictions addressRestrictions = { };
	addressRestrictions.alignment = 0x100000;

	while (nsSize > 0x4000000) {
		if (ReserveAddressRange(nsSize, &addressRestrictions, 0,
				CACHE_DONT_LOCK_KERNEL |
				CACHE_DONT_WAIT_FOR_MEMORY, &base) == 0) {
			break;
		}

		nsSize >>= 1;
	}

	if (!base) {
		panic("Can't allocate NS reservation");
	}

	fNSRangeBase = (uintptr_t) base;
	fNSRangeEnd = (uintptr_t) base + nsSize;

	Range* range = fRangeTree.FindClosest(fNSRangeBase, false);

	if(!range)
		panic("Memory range not found");

	// Set range type to Non-Secure - this way it can't be merged with others
	range->ns = true;

	UnreserveAddressRange(fNSRangeBase, nsSize, CACHE_DONT_LOCK_KERNEL |
			CACHE_DONT_WAIT_FOR_MEMORY);
}
#endif

inline VMArea*
VMKernelAddressSpace::FirstArea() const
{
	Range* range = fRangeList.Head();
	while (range != nullptr && range->type != Range::RANGE_AREA)
		range = fRangeList.GetNext(range);
	return range != nullptr ? range->area : nullptr;
}


inline VMArea*
VMKernelAddressSpace::NextArea(VMArea* _area) const
{
	Range* range = static_cast<VMKernelArea*>(_area)->Range();
	ASSERT(range != nullptr);

	do {
		range = fRangeList.GetNext(range);
	} while (range != nullptr && range->type != Range::RANGE_AREA);
	return range != nullptr ? range->area : nullptr;
}


VMArea*
VMKernelAddressSpace::CreateArea(const char* name, uint32_t wiring,
	uint32_t protection, uint32_t allocationFlags)
{
	return VMKernelArea::Create(this, name, wiring, protection,
		fAreaObjectCache, allocationFlags);
}


void
VMKernelAddressSpace::DeleteArea(VMArea* _area, uint32_t)
{
	TRACE("VMKernelAddressSpace::DeleteArea(%p)\n", _area);

	VMKernelArea* area = static_cast<VMKernelArea*>(_area);
	object_cache_delete(fAreaObjectCache, area);
}


//! You must hold the address space's read lock.
VMArea*
VMKernelAddressSpace::LookupArea(uintptr_t address) const
{
	Range* range = fRangeTree.FindClosest(address, true);
	if (range == nullptr || range->type != Range::RANGE_AREA)
		return nullptr;

	VMKernelArea* area = range->area;
	return area->ContainsAddress(address) ? area : nullptr;
}

VMArea*
VMKernelAddressSpace::FindClosestArea(uintptr_t address, bool less) const
{
	Range* range = fRangeTree.FindClosest(address, less);
	while (range != NULL && range->type != Range::RANGE_AREA)
		range = fRangeTree.Next(range);

	return range != NULL ? range->area : NULL;
}

/*!	This inserts the area you pass into the address space.
	It will also set the "_address" argument to its base address when
	the call succeeds.
	You need to hold the VMAddressSpace write lock.
*/
int
VMKernelAddressSpace::InsertArea(VMArea* _area, size_t size,
	const virtual_address_restrictions* addressRestrictions,
	uint32_t allocationFlags, void** _address)
{
	TRACE("VMKernelAddressSpace::InsertArea(%p, %" PRIu32 ", %zx"
		", %p \"%s\")\n", addressRestrictions->address,
		addressRestrictions->address_specification, size, _area, _area->name);

	VMKernelArea* area = static_cast<VMKernelArea*>(_area);

	Range* range;
	int error = _AllocateRange(addressRestrictions, size,
		addressRestrictions->address_specification == B_EXACT_ADDRESS,
		allocationFlags, range
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		, (area->protection & B_NONSECURE_AREA) != 0
#endif
		);
	if (error != 0)
		return error;

	range->type = Range::RANGE_AREA;
	range->area = area;
	area->SetRange(range);
	area->SetBase(range->base);
	area->SetSize(range->size);
	area->SetCommitBase(range->base + range->size);

	if (_address != nullptr)
		*_address = (void*)area->Base();
	fFreeSpace -= area->Size();

	PARANOIA_CHECK_STRUCTURES();

	return 0;
}


//! You must hold the address space's write lock.
void
VMKernelAddressSpace::RemoveArea(VMArea* _area, uint32_t allocationFlags)
{
	TRACE("VMKernelAddressSpace::RemoveArea(%p)\n", _area);

	VMKernelArea* area = static_cast<VMKernelArea*>(_area);

	ASSERT(area->Range() != nullptr);
	_FreeRange(area->Range(), allocationFlags);
	fFreeSpace += area->Size();

	PARANOIA_CHECK_STRUCTURES();
}


bool
VMKernelAddressSpace::CanResizeArea(VMArea* area, size_t newSize)
{
	Range* range = static_cast<VMKernelArea*>(area)->Range();
	ASSERT(range != nullptr);

	if (newSize <= range->size)
		return true;

	Range* nextRange = fRangeList.GetNext(range);
	if (nextRange == nullptr || nextRange->type == Range::RANGE_AREA)
		return false;

	if (nextRange->type == Range::RANGE_RESERVED
		&& nextRange->reserved.base > range->base) {
		return false;
	}

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	if (nextRange->ns != range->ns && area->Base() + newSize >= nextRange->base)
		return false;
#endif

	// TODO: If there is free space after a reserved range (or vice versa), it
	// could be used as well.
	return newSize - range->size <= nextRange->size;
}


int
VMKernelAddressSpace::ResizeArea(VMArea* _area, size_t newSize,
	uint32_t allocationFlags)
{
	TRACE("VMKernelAddressSpace::ResizeArea(%p, %zx)\n", _area,
		newSize);

	VMKernelArea* area = static_cast<VMKernelArea*>(_area);
	Range* range = area->Range();
	ASSERT(range != nullptr);

	if (newSize == range->size)
		return 0;

	Range* nextRange = fRangeList.GetNext(range);

	if (newSize < range->size) {
		if (nextRange != nullptr && nextRange->type == Range::RANGE_FREE) {
			// a free range is following -- just enlarge it
			_FreeListRemoveRange(nextRange, nextRange->size);
			nextRange->size += range->size - newSize;
			nextRange->base = range->base + newSize;
			_FreeListInsertRange(nextRange, nextRange->size);
		} else {
			// no free range following -- we need to allocate a new one and
			// insert it
			nextRange = new(fRangesObjectCache, allocationFlags) Range(
				range->base + newSize, range->size - newSize,
				Range::RANGE_FREE);
			if (nextRange == nullptr)
				return -ENOMEM;
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			nextRange->ns = range->ns;
#endif
			_InsertRange(nextRange);
		}
	} else {
		if (nextRange == nullptr
			|| (nextRange->type == Range::RANGE_RESERVED
				&& nextRange->reserved.base > range->base)) {
			return -EINVAL;
		}
		// TODO: If there is free space after a reserved range (or vice versa),
		// it could be used as well.
		size_t sizeDiff = newSize - range->size;
		if (sizeDiff > nextRange->size)
			return -EINVAL;

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		if (nextRange->ns != range->ns && area->Base() + newSize >= nextRange->base)
			return -EINVAL;
#endif

		if (sizeDiff == nextRange->size) {
			// The next range is completely covered -- remove and delete it.
			_RemoveRange(nextRange);
			object_cache_delete(fRangesObjectCache, nextRange, allocationFlags);
		} else {
			// The next range is only partially covered -- shrink it.
			if (nextRange->type == Range::RANGE_FREE)
				_FreeListRemoveRange(nextRange, nextRange->size);
			nextRange->size -= sizeDiff;
			nextRange->base = range->base + newSize;
			if (nextRange->type == Range::RANGE_FREE)
				_FreeListInsertRange(nextRange, nextRange->size);
		}
	}

	range->size = newSize;
	area->SetSize(newSize);
	area->SetCommitBase(std::min(area->CommitBase(), area->Base() + newSize));

	IncrementChangeCount();
	PARANOIA_CHECK_STRUCTURES();
	return 0;
}


int
VMKernelAddressSpace::ShrinkAreaHead(VMArea* _area, size_t newSize,
	uint32_t allocationFlags)
{
	TRACE("VMKernelAddressSpace::ShrinkAreaHead(%p, %zx)\n", _area,
		newSize);

	VMKernelArea* area = static_cast<VMKernelArea*>(_area);
	Range* range = area->Range();
	ASSERT(range != nullptr);

	if (newSize == range->size)
		return 0;

	if (newSize > range->size)
		return -EINVAL;

	Range* previousRange = fRangeList.GetPrevious(range);

	size_t sizeDiff = range->size - newSize;
	if (previousRange != nullptr && previousRange->type == Range::RANGE_FREE) {
		// the previous range is free -- just enlarge it
		_FreeListRemoveRange(previousRange, previousRange->size);
		previousRange->size += sizeDiff;
		_FreeListInsertRange(previousRange, previousRange->size);
		range->base += sizeDiff;
		range->size = newSize;
	} else {
		// no free range before -- we need to allocate a new one and
		// insert it
		previousRange = new(fRangesObjectCache, allocationFlags) Range(
			range->base, sizeDiff, Range::RANGE_FREE);
		if (previousRange == nullptr)
			return -ENOMEM;
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		previousRange->ns = range->ns;
#endif
		range->base += sizeDiff;
		range->size = newSize;
		_InsertRange(previousRange);
	}

	area->SetBase(range->base);
	area->SetSize(range->size);
	area->SetCommitBase(std::max(range->base, area->CommitBase()));

	IncrementChangeCount();
	PARANOIA_CHECK_STRUCTURES();
	return 0;
}


int
VMKernelAddressSpace::ShrinkAreaTail(VMArea* area, size_t newSize,
	uint32_t allocationFlags)
{
	return ResizeArea(area, newSize, allocationFlags);
}


int
VMKernelAddressSpace::ReserveAddressRange(size_t size,
	const virtual_address_restrictions* addressRestrictions,
	uint32_t flags, uint32_t allocationFlags, void** _address)
{
	TRACE("VMKernelAddressSpace::ReserveAddressRange(%p, %" PRIu32 ", %"
		"zx, %#" PRIx32 ")\n", addressRestrictions->address,
		addressRestrictions->address_specification, size, flags);

	// Don't allow range reservations, if the address space is about to be
	// deleted.
	if (fDeleting)
		return -ESRCH;

	Range* range;
	int error = _AllocateRange(addressRestrictions, size, false,
		allocationFlags, range
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		, false
#endif
		);

	if (error != 0)
		return error;

	range->type = Range::RANGE_RESERVED;
	range->reserved.base = range->base;
	range->reserved.flags = flags;

	if (_address != nullptr)
		*_address = (void*)range->base;

	Get();
	PARANOIA_CHECK_STRUCTURES();
	return 0;
}


int
VMKernelAddressSpace::UnreserveAddressRange(uintptr_t address, size_t size,
	uint32_t allocationFlags)
{
	TRACE("VMKernelAddressSpace::UnreserveAddressRange(%#" PRIxPTR ", %zx"
		")\n", address, size);

	// Don't allow range unreservations, if the address space is about to be
	// deleted. UnreserveAllAddressRanges() must be used.
	if (fDeleting)
		return -ESRCH;

	// search range list and remove any matching reserved ranges
	uintptr_t endAddress = address + (size - 1);
	Range* range = fRangeTree.FindClosest(address, false);
	while (range != nullptr && range->base + (range->size - 1) <= endAddress) {
		// Get the next range for the iteration -- we need to skip free ranges,
		// since _FreeRange() might join them with the current range and delete
		// them.
		Range* nextRange = fRangeList.GetNext(range);
		while (nextRange != nullptr && nextRange->type == Range::RANGE_FREE)
			nextRange = fRangeList.GetNext(nextRange);

		if (range->type == Range::RANGE_RESERVED) {
			_FreeRange(range, allocationFlags);
			Put();
		}

		range = nextRange;
	}

	PARANOIA_CHECK_STRUCTURES();
	return 0;
}


void
VMKernelAddressSpace::UnreserveAllAddressRanges(uint32_t allocationFlags)
{
	Range* range = fRangeList.Head();
	while (range != nullptr) {
		// Get the next range for the iteration -- we need to skip free ranges,
		// since _FreeRange() might join them with the current range and delete
		// them.
		Range* nextRange = fRangeList.GetNext(range);
		while (nextRange != nullptr && nextRange->type == Range::RANGE_FREE)
			nextRange = fRangeList.GetNext(nextRange);

		if (range->type == Range::RANGE_RESERVED) {
			_FreeRange(range, allocationFlags);
			Put();
		}

		range = nextRange;
	}

	PARANOIA_CHECK_STRUCTURES();
}


inline void
VMKernelAddressSpace::_FreeListInsertRange(Range* range, size_t size)
{
	TRACE("    VMKernelAddressSpace::_FreeListInsertRange(%p (%#" PRIxPTR
		", %zx, %d), %zx (%d))\n", range, range->base,
		range->size, range->type, size, ld(size) - PAGE_SHIFT);

	fFreeLists[ld(size) - PAGE_SHIFT].Add(range);
}


inline void
VMKernelAddressSpace::_FreeListRemoveRange(Range* range, size_t size)
{
	TRACE("    VMKernelAddressSpace::_FreeListRemoveRange(%p (%#" PRIxPTR
		", %zx, %d), %zx (%d))\n", range, range->base,
		range->size, range->type, size, ld(size) - PAGE_SHIFT);

	fFreeLists[ld(size) - PAGE_SHIFT].Remove(range);
}


void
VMKernelAddressSpace::_InsertRange(Range* range)
{
	TRACE("    VMKernelAddressSpace::_InsertRange(%p (%#" PRIxPTR ", %zx"
		", %d))\n", range, range->base, range->size, range->type);

	// insert at the correct position in the range list
	Range* insertBeforeRange = fRangeTree.FindClosest(range->base, true);
	fRangeList.Insert(
		insertBeforeRange != nullptr
			? fRangeList.GetNext(insertBeforeRange) : fRangeList.Head(),
		range);

	// insert into tree
	fRangeTree.Insert(range);

	// insert in the free ranges list, if the range is free
	if (range->type == Range::RANGE_FREE)
		_FreeListInsertRange(range, range->size);
}


void
VMKernelAddressSpace::_RemoveRange(Range* range)
{
	TRACE("    VMKernelAddressSpace::_RemoveRange(%p (%" PRIxPTR ", %zx"
		", %d))\n", range, range->base, range->size, range->type);

	// remove from tree and range list
	// insert at the correct position in the range list
	fRangeTree.Remove(range);
	fRangeList.Remove(range);

	// if it is a free range, also remove it from the free list
	if (range->type == Range::RANGE_FREE)
		_FreeListRemoveRange(range, range->size);
}


int
VMKernelAddressSpace::_AllocateRange(
	const virtual_address_restrictions* addressRestrictions,
	size_t size, bool allowReservedRange, uint32_t allocationFlags,
	Range*& _range
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	, bool nonSecure
#endif
	)
{
	TRACE("  VMKernelAddressSpace::_AllocateRange(address: %p, size: %zx"
		", addressSpec: %" PRIx32 ", reserved allowed: %d)\n",
		addressRestrictions->address, size,
		addressRestrictions->address_specification, allowReservedRange);

	// prepare size, alignment and the base address for the range search
	uintptr_t address = (uintptr_t)addressRestrictions->address;
	size = roundup2(size, PAGE_SIZE);
	size_t alignment = addressRestrictions->alignment != 0
		? addressRestrictions->alignment : PAGE_SIZE;
	size_t maximumAnyAlignment = TranslationMap()->HardwareBlockSize();
	uint32_t addressSpecification = addressRestrictions->address_specification;

	switch (addressSpecification) {
		case B_EXACT_ADDRESS:
		{
			if (address % PAGE_SIZE != 0)
				return -EINVAL;

			break;
		}

		case B_RANDOMIZED_BASE_ADDRESS:
		case B_BASE_ADDRESS:
			address = roundup2(address, PAGE_SIZE);
			break;

		case B_ANY_KERNEL_BLOCK_ADDRESS:
			// align the memory to the next power of two of the size
			while (alignment < size && alignment < maximumAnyAlignment)
				alignment <<= 1;

			address = fBase;
			addressSpecification = B_RANDOMIZED_ANY_ADDRESS;
			break;

		case B_RANDOMIZED_ANY_ADDRESS:
		case B_ANY_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
			address = fBase;
			break;

		default:
			return -EINVAL;
	}

	// find a range
	Range* range = _FindFreeRange(address, size, alignment,
			addressSpecification, allowReservedRange,
			address
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			, nonSecure
#endif
			);

	if (range == nullptr) {
		return addressRestrictions->address_specification == B_EXACT_ADDRESS
			? -EINVAL : -ENOMEM;
	}

	TRACE("  VMKernelAddressSpace::_AllocateRange() found range:(%p (%"
		PRIxPTR ", %zx, %d)\n", range, range->base, range->size,
		range->type);

	// We have found a range. It might not be a perfect fit, in which case
	// we have to split the range.
	size_t rangeSize = range->size;

	if (address == range->base) {
		// allocation at the beginning of the range
		if (range->size > size) {
			// only partial -- split the range
			Range* leftOverRange = new(fRangesObjectCache, allocationFlags)
				Range(address + size, range->size - size, range);
			if (leftOverRange == nullptr)
				return -ENOMEM;

			range->size = size;
			_InsertRange(leftOverRange);
		}
	} else if (address + size == range->base + range->size) {
		// allocation at the end of the range -- split the range
		Range* leftOverRange = new(fRangesObjectCache, allocationFlags) Range(
			range->base, range->size - size, range);
		if (leftOverRange == nullptr)
			return -ENOMEM;

		range->base = address;
		range->size = size;
		_InsertRange(leftOverRange);
	} else {
		// allocation in the middle of the range -- split the range in three
		Range* leftOverRange1 = new(fRangesObjectCache, allocationFlags) Range(
			range->base, address - range->base, range);
		if (leftOverRange1 == nullptr)
			return -ENOMEM;
		Range* leftOverRange2 = new(fRangesObjectCache, allocationFlags) Range(
			address + size, range->size - size - leftOverRange1->size, range);
		if (leftOverRange2 == nullptr) {
			object_cache_delete(fRangesObjectCache, leftOverRange1,
				allocationFlags);
			return -ENOMEM;
		}

		range->base = address;
		range->size = size;
		_InsertRange(leftOverRange1);
		_InsertRange(leftOverRange2);
	}

	// If the range is a free range, remove it from the respective free list.
	if (range->type == Range::RANGE_FREE)
		_FreeListRemoveRange(range, rangeSize);

	IncrementChangeCount();

	TRACE("  VMKernelAddressSpace::_AllocateRange() -> %p (%#" PRIxPTR ", %zx"
		")\n", range, range->base, range->size);

	_range = range;
	return 0;
}


VMKernelAddressSpace::Range*
VMKernelAddressSpace::_FindFreeRange(uintptr_t start, size_t size,
	size_t alignment, uint32_t addressSpec, bool allowReservedRange,
	uintptr_t& _foundAddress
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	, bool nonSecure
#endif
	)
{
	TRACE("  VMKernelAddressSpace::_FindFreeRange(start: %" PRIxPTR
		", size: %zx, alignment: %zx, addressSpec: %"
		PRIx32 ", reserved allowed: %d)\n", start, size, alignment,
		addressSpec, allowReservedRange);

	if(addressSpec == B_RANDOMIZED_BASE_ADDRESS || addressSpec == B_BASE_ADDRESS) {
		if(start < fBase)
			start = fBase;
		else if(start >= fEndAddress)
			start = fBase;
		else if(fEndAddress + 1 - start < size)
			start = fBase;

		start &= ~(alignment - 1);
	}

	uintptr_t originalStart = start;

	if(IsRandomizingEnabled() && addressSpec == B_RANDOMIZED_BASE_ADDRESS) {
		start = _RandomizeAddress(start, fEndAddress - size, alignment, true);
	}

second_chance:
	switch (addressSpec) {
		case B_RANDOMIZED_BASE_ADDRESS:
		case B_BASE_ADDRESS:
		{
			// We have to iterate through the range list starting at the given
			// address. This is the most inefficient case.
			Range* range = fRangeTree.FindClosest(start, true);
			while (range != nullptr) {
				if (range->type == Range::RANGE_FREE
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
						&& range->ns == nonSecure
#endif
						) {
					uintptr_t alignedBase = roundup2(range->base, alignment);
					uintptr_t nextBase = range->base + (range->size - 1);
					if (is_valid_spot(start, alignedBase, size, nextBase)) {
						if (_IsRandomized(addressSpec)) {
							alignedBase = _RandomizeAddress(alignedBase,
									nextBase - size + 1, alignment);
						}

						_foundAddress = alignedBase;
						return range;
					}
				}
				range = fRangeList.GetNext(range);
			}
			// We didn't find a free spot in the requested range, so we'll
			// try again without any restrictions.
		}

			[[fallthrough]];

		case B_ANY_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
		case B_RANDOMIZED_ANY_ADDRESS:
		{
			// We want to allocate from the first non-empty free list that is
			// guaranteed to contain the size. Finding a free range is O(1),
			// unless there are constraints (min base address, alignment).
			int freeListIndex = ld((size * 2 - 1) >> PAGE_SHIFT);

			for (int32_t i = freeListIndex; i < fFreeListCount; i++) {
				RangeFreeList& freeList = fFreeLists[i];
				if (freeList.IsEmpty())
					continue;

				for (RangeFreeList::Iterator it = freeList.GetIterator();
						Range* range = it.Next();) {
					uintptr_t alignedBase = roundup2(range->base, alignment);
					uintptr_t nextBase = range->base + (range->size - 1);
					if (is_valid_spot(start, alignedBase, size, nextBase)) {
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
						if(range->ns != nonSecure)
							continue;
#endif

						if (_IsRandomized(addressSpec)) {
							alignedBase = _RandomizeAddress(alignedBase,
									nextBase - size + 1, alignment);
						}

						_foundAddress = alignedBase;
						return range;
					}
				}
			}

			if (!allowReservedRange) {
				if(addressSpec == B_RANDOMIZED_BASE_ADDRESS) {
					if(start == originalStart) {
						start = fBase;
						addressSpec = B_RANDOMIZED_ANY_ADDRESS;
					} else {
						start = originalStart;
						addressSpec = B_RANDOMIZED_BASE_ADDRESS;
					}

					goto second_chance;
				}

				return nullptr;
			}

			// We haven't found any free ranges, but we're supposed to look
			// for reserved ones, too. Iterate through the range list starting
			// at the given address.
			Range* range = fRangeTree.FindClosest(start, true);
			while (range != nullptr) {
				if (range->type == Range::RANGE_RESERVED
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
						&& range->ns == nonSecure
#endif
						) {
					uintptr_t alignedBase = roundup2(range->base, alignment);
					uintptr_t nextBase = range->base + (range->size - 1);
					if (is_valid_spot(start, alignedBase, size, nextBase)) {
						// allocation from the back might be preferred
						// -- adjust the base accordingly
						if ((range->reserved.flags & RESERVED_AVOID_BASE)
								!= 0) {
							alignedBase = rounddown2(
								range->base + (range->size - size), alignment);
						} else {
							if (_IsRandomized(addressSpec)) {
								alignedBase = _RandomizeAddress(alignedBase,
										nextBase - size + 1, alignment);
							}
						}

						_foundAddress = alignedBase;
						return range;
					}
				}
				range = fRangeList.GetNext(range);
			}

			if(addressSpec == B_RANDOMIZED_BASE_ADDRESS) {
				if(start == originalStart) {
					start = fBase;
					addressSpec = B_RANDOMIZED_ANY_ADDRESS;
				} else {
					start = originalStart;
					addressSpec = B_RANDOMIZED_BASE_ADDRESS;
				}

				goto second_chance;
			}

			return nullptr;
		}

		case B_EXACT_ADDRESS:
		{
			Range* range = fRangeTree.FindClosest(start, true);
TRACE("    B_EXACT_ADDRESS: range: %p\n", range);
			if (range == nullptr || range->type == Range::RANGE_AREA
				|| range->base + (range->size - 1) < start + (size - 1)) {
				// TODO: Support allocating if the area range covers multiple
				// free and reserved ranges!
TRACE("    -> no suitable range\n");
				return nullptr;
			}

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			if (range->ns != nonSecure)
				return NULL;
#endif

			if (range->type != Range::RANGE_FREE && !allowReservedRange)
{
TRACE("    -> reserved range not allowed\n");
				return nullptr;
}

			_foundAddress = start;
			return range;
		}

		default:
			return nullptr;
	}
}


void
VMKernelAddressSpace::_FreeRange(Range* range, uint32_t allocationFlags)
{
	TRACE("  VMKernelAddressSpace::_FreeRange(%p (%" PRIxPTR ", %zx"
		", %d))\n", range, range->base, range->size, range->type);

	// Check whether one or both of the neighboring ranges are free already,
	// and join them, if so.
	Range* previousRange = fRangeList.GetPrevious(range);
	Range* nextRange = fRangeList.GetNext(range);

	if (previousRange != nullptr && previousRange->type == Range::RANGE_FREE
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
			&& previousRange->ns == range->ns
#endif
			) {
		if (nextRange != nullptr && nextRange->type == Range::RANGE_FREE
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
				&& nextRange->ns == range->ns
#endif
				) {
			// join them all -- keep the first one, delete the others
			_FreeListRemoveRange(previousRange, previousRange->size);
			_RemoveRange(range);
			_RemoveRange(nextRange);
			previousRange->size += range->size + nextRange->size;
			object_cache_delete(fRangesObjectCache, range, allocationFlags);
			object_cache_delete(fRangesObjectCache, nextRange, allocationFlags);
			_FreeListInsertRange(previousRange, previousRange->size);
		} else {
			// join with the previous range only, delete the supplied one
			_FreeListRemoveRange(previousRange, previousRange->size);
			_RemoveRange(range);
			previousRange->size += range->size;
			object_cache_delete(fRangesObjectCache, range, allocationFlags);
			_FreeListInsertRange(previousRange, previousRange->size);
		}
	} else {
		if (nextRange != nullptr && nextRange->type == Range::RANGE_FREE
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
				&& nextRange->ns == range->ns
#endif
				) {
			// join with the next range and delete it
			_RemoveRange(nextRange);
			range->size += nextRange->size;
			object_cache_delete(fRangesObjectCache, nextRange, allocationFlags);
		}

		// mark the range free and add it to the respective free list
		range->type = Range::RANGE_FREE;
		_FreeListInsertRange(range, range->size);
	}

	IncrementChangeCount();
}


#ifdef PARANOIA_CHECKS

void
VMKernelAddressSpace::_CheckStructures() const
{
	// check range list and tree
	size_t spaceSize = fEndAddress - fBase + 1;
	uintptr_t nextBase = fBase;
	Range* previousRange = NULL;
	int previousRangeType = Range::RANGE_AREA;
	uint64_t freeRanges = 0;

	RangeList::ConstIterator listIt = fRangeList.GetIterator();
	RangeTree::ConstIterator treeIt = fRangeTree.GetIterator();
	while (true) {
		Range* range = listIt.Next();
		Range* treeRange = treeIt.Next();
		if (range != treeRange) {
			panic("VMKernelAddressSpace::_CheckStructures(): list/tree range "
				"mismatch: %p vs %p", range, treeRange);
		}
		if (range == NULL)
			break;

		if (range->base != nextBase) {
			panic("VMKernelAddressSpace::_CheckStructures(): range base %#"
				PRIxPTR ", expected: %#" PRIxPTR, range->base, nextBase);
		}

		if (range->size == 0) {
			panic("VMKernelAddressSpace::_CheckStructures(): empty range %p",
				range);
		}

		if (range->size % PAGE_SIZE != 0) {
			panic("VMKernelAddressSpace::_CheckStructures(): range %p (%"
				PRIxPTR ", %zx) not page aligned", range,
				range->base, range->size);
		}

		if (range->size > spaceSize - (range->base - fBase)) {
			panic("VMKernelAddressSpace::_CheckStructures(): range too large: "
				"(%" PRIxPTR ", %zx), address space end: %#"
				PRIxPTR, range->base, range->size, fEndAddress);
		}

		if (range->type == Range::RANGE_FREE) {
			freeRanges++;

			if (previousRangeType == Range::RANGE_FREE) {
				panic("VMKernelAddressSpace::_CheckStructures(): adjoining "
					"free ranges: %p (%" PRIxPTR ", %zx"
					"), %p (%" PRIxPTR ", %zx)", previousRange,
					previousRange->base, previousRange->size, range,
					range->base, range->size);
			}
		}

		previousRange = range;
		nextBase = range->base + range->size;
		previousRangeType = range->type;
	}

	if (nextBase - 1 != fEndAddress) {
		panic("VMKernelAddressSpace::_CheckStructures(): space not fully "
			"covered by ranges: last: %" PRIxPTR ", expected %" PRIxPTR,
			nextBase - 1, fEndAddress);
	}

	// check free lists
	uint64_t freeListRanges = 0;
	for (int i = 0; i < fFreeListCount; i++) {
		RangeFreeList& freeList = fFreeLists[i];
		if (freeList.IsEmpty())
			continue;

		for (RangeFreeList::Iterator it = freeList.GetIterator();
				Range* range = it.Next();) {
			if (range->type != Range::RANGE_FREE) {
				panic("VMKernelAddressSpace::_CheckStructures(): non-free "
					"range %p (%" PRIxPTR ", %zx, %d) in "
					"free list %d", range, range->base, range->size,
					range->type, i);
			}

			if (fRangeTree.Find(range->base) != range) {
				panic("VMKernelAddressSpace::_CheckStructures(): unknown "
					"range %p (%" PRIxPTR ", %zx, %d) in "
					"free list %d", range, range->base, range->size,
					range->type, i);
			}

			if (ld(range->size) - PAGE_SHIFT != i) {
				panic("VMKernelAddressSpace::_CheckStructures(): "
					"range %p (%" PRIxPTR ", %zx, %d) in "
					"wrong free list %d", range, range->base, range->size,
					range->type, i);
			}

			freeListRanges++;
		}
	}
}

#endif	// PARANOIA_CHECKS

void
VMKernelAddressSpace::Dump()
{
#if defined(CONFIG_KDEBUGGER)
	VMAddressSpace::Dump();

	kprintf("area_list:\n");

	for (auto it = fRangeList.GetIterator();
			Range* range = it.Next();) {
		if (range->type != Range::RANGE_AREA)
			continue;

		VMKernelArea* area = range->area;
		kprintf(" area %" PRId32 ": ", area->id);
		kprintf("base_addr = %#" PRIxPTR " ", area->Base());
		kprintf("size = %zx ", area->Size());
		kprintf("name = '%s' ", area->name);
		kprintf("protection = %#" PRIx32 "\n", area->protection);
	}
#endif
}
