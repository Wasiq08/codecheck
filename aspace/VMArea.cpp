

#include <mm/VMArea.h>
#include <mm/VMAddressSpace.h>
#include <cinttypes>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <core/random.h>

#define AREA_HASH_TABLE_SIZE 1024


rw_lock_t VMAreaHash::sLock = RW_LOCK_INITIALIZER("area hash");

VMAreaHashTable VMAreaHash::sTable;

static area_id sNextAreaId = 0;

// #pragma mark - VMArea

VMArea::VMArea(VMAddressSpace* addressSpace, uint32_t areaWiring, uint32_t areaProtection)
	:
	id(-1),
	protection(areaProtection),
	wiring(areaWiring),
	cache(nullptr),
	cache_offset(0),
	cache_type(0),
	page_protections(nullptr),
	address_space(addressSpace),
	cache_next(nullptr),
	cache_prev(nullptr),
	hash_next(nullptr),
	fBase(0),
	fSize(0),
	fCommitBase(0)
{
	name[0] = '\0';
}


VMArea::~VMArea()
{
	const uint32_t flags = CACHE_DONT_LOCK_KERNEL | CACHE_DONT_WAIT_FOR_MEMORY;
	free_etc(page_protections, flags);
}


void
VMArea::Init(const char* areaName)
{
	// copy the name
	strlcpy(this->name, areaName, B_OS_NAME_LENGTH);
}


/*!	Returns whether any part of the given address range intersects with a wired
	range of this area.
	The area's top cache must be locked.
*/
bool
VMArea::IsWired(uintptr_t base, size_t size) const
{
	for (VMAreaWiredRangeList::Iterator it = fWiredRanges.GetIterator();
			VMAreaWiredRange* range = it.Next();) {
		if (range->IntersectsWith(base, size))
			return true;
	}

	return false;
}


/*!	Adds the given wired range to this area.
	The area's top cache must be locked.
*/
void
VMArea::Wire(VMAreaWiredRange* range)
{
	ASSERT(range->area == nullptr);

	range->area = this;
	fWiredRanges.Add(range);
}


/*!	Removes the given wired range from this area.
	Must balance a previous Wire() call.
	The area's top cache must be locked.
*/
void
VMArea::Unwire(VMAreaWiredRange* range)
{
	ASSERT(range->area == this);

	// remove the range
	range->area = nullptr;
	fWiredRanges.Remove(range);

	// wake up waiters
	for (VMAreaUnwiredWaiterList::Iterator it = range->waiters.GetIterator();
			VMAreaUnwiredWaiter* waiter = it.Next();) {
		waiter->condition.NotifyAll();
	}

	range->waiters.MakeEmpty();
}


/*!	Removes a wired range from this area.

	Must balance a previous Wire() call. The first implicit range with matching
	\a base, \a size, and \a writable attributes is removed and returned. It's
	waiters are woken up as well.
	The area's top cache must be locked.
*/
VMAreaWiredRange*
VMArea::Unwire(uintptr_t base, size_t size, bool writable)
{
	for (VMAreaWiredRangeList::Iterator it = fWiredRanges.GetIterator();
			VMAreaWiredRange* range = it.Next();) {
		if (range->implicit && range->base == base && range->size == size
				&& range->writable == writable) {
			Unwire(range);
			return range;
		}
	}

	panic("VMArea::Unwire(%#" PRIxPTR ", %#" PRIxPTR ", %d): no such "
		"range", base, size, writable);
}


/*!	If the area has any wired range, the given waiter is added to the range and
	prepared for waiting.

	\return \c true, if the waiter has been added, \c false otherwise.
*/
bool
VMArea::AddWaiterIfWired(VMAreaUnwiredWaiter* waiter)
{
	VMAreaWiredRange* range = fWiredRanges.Head();
	if (range == nullptr)
		return false;

	waiter->area = this;
	waiter->base = fBase;
	waiter->size = fSize;
	waiter->condition.Init(this, "area unwired");
	waiter->condition.Add(&waiter->waitEntry);

	range->waiters.Add(waiter);

	return true;
}


/*!	If the given address range intersect with a wired range of this area, the
	given waiter is added to the range and prepared for waiting.

	\param waiter The waiter structure that will be added to the wired range
		that intersects with the given address range.
	\param base The base of the address range to check.
	\param size The size of the address range to check.
	\param flags
		- \c IGNORE_WRITE_WIRED_RANGES: Ignore ranges wired for writing.
	\return \c true, if the waiter has been added, \c false otherwise.
*/
bool
VMArea::AddWaiterIfWired(VMAreaUnwiredWaiter* waiter, uintptr_t base, size_t size,
	uint32_t flags)
{
	for (VMAreaWiredRangeList::Iterator it = fWiredRanges.GetIterator();
			VMAreaWiredRange* range = it.Next();) {
		if ((flags & IGNORE_WRITE_WIRED_RANGES) != 0 && range->writable)
			continue;

		if (range->IntersectsWith(base, size)) {
			waiter->area = this;
			waiter->base = base;
			waiter->size = size;
			waiter->condition.Init(this, "area unwired");
			waiter->condition.Add(&waiter->waitEntry);

			range->waiters.Add(waiter);

			return true;
		}
	}

	return false;
}


// #pragma mark - VMAreaHash


/*static*/ int
VMAreaHash::Init()
{
	return sTable.Init(AREA_HASH_TABLE_SIZE);
}


/*static*/ VMArea*
VMAreaHash::Lookup(area_id id)
{
	ReadLock();
	VMArea* area = LookupLocked(id);
	ReadUnlock();
	return area;
}

VMArea*
VMAreaHash::Lookup(area_id id, VMAddressSpace * addressSpace)
{
	ReadLock();
	VMArea* area = LookupLocked(id);
	if(area && area->address_space != addressSpace) {
		area = nullptr;
	}
	ReadUnlock();
	return area;
}

/*static*/ area_id
VMAreaHash::Find(const char* name)
{
	ReadLock();

	area_id id = -ENOENT;

	// TODO: Iterating through the whole table can be very slow and the whole
	// time we're holding the lock! Use a second hash table!

	for (VMAreaHashTable::Iterator it = sTable.GetIterator();
			VMArea* area = it.Next();) {
		if (strcmp(area->name, name) == 0) {
			id = area->id;
			break;
		}
	}

	ReadUnlock();

	return id;
}


/*static*/ void
VMAreaHash::Insert(VMArea* area)
{
	WriteLock();

	do {
		++sNextAreaId;

		if(sNextAreaId < 1)
			sNextAreaId = 1;
		else if(sNextAreaId > (INT32_MAX - 0xf))
			sNextAreaId = 1;
	} while(sTable.Lookup(sNextAreaId));

	area->id = sNextAreaId;
	sTable.InsertUnchecked(area);
	WriteUnlock();
}


/*static*/ void
VMAreaHash::Remove(VMArea* area)
{
	WriteLock();
	sTable.RemoveUnchecked(area);
	WriteUnlock();
}
