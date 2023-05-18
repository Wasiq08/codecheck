#include <core/debug.h>
#include <mm/mm_locking.h>
#include <mm/xmalloc.h>
#include <mm/mm_cache.h>
#include <trustware/AutoDeleter.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <algorithm>

//	#pragma mark - AddressSpaceLockerBase

VMAddressSpace* AddressSpaceLockerBase::GetAddressSpaceByAreaID(int id) {
	VMAddressSpace *addressSpace = nullptr;

	VMAreaHash::ReadLock();

	VMArea *area = VMAreaHash::LookupLocked(id);

	if (area != nullptr) {
		addressSpace = area->address_space;
		addressSpace->Get();
	}

	VMAreaHash::ReadUnlock();

	return addressSpace;
}

//	#pragma mark - AddressSpaceReadLocker

AddressSpaceReadLocker::AddressSpaceReadLocker(uint32_t team) :
		fSpace(nullptr), fLocked(false) {
	SetTo(team);
}

/*! Takes over the reference of the address space, if \a getNewReference is
 \c false.
 */
AddressSpaceReadLocker::AddressSpaceReadLocker(VMAddressSpace *space,
		bool getNewReference) :
		fSpace(nullptr), fLocked(false) {
	SetTo(space, getNewReference);
}

AddressSpaceReadLocker::~AddressSpaceReadLocker() {
	Unset();
}

void AddressSpaceReadLocker::Unset() {
	Unlock();
	if (fSpace != nullptr) {
		fSpace->Put();
		fSpace = nullptr;
	}
}

int AddressSpaceReadLocker::SetTo(uint32_t team) {
	Unset();

	fSpace = VMAddressSpace::Get(team);
	if (fSpace == nullptr)
		return -ESRCH;

	fSpace->ReadLock();
	fLocked = true;
	return 0;
}

/*! Takes over the reference of the address space, if \a getNewReference is
 \c false.
 */
void AddressSpaceReadLocker::SetTo(VMAddressSpace *space,
		bool getNewReference) {
	Unset();

	fSpace = space;

	if (getNewReference)
		fSpace->Get();

	fSpace->ReadLock();
	fLocked = true;
}

int AddressSpaceReadLocker::SetFromArea(int areaID, VMArea *&area) {
	Unset();

	fSpace = GetAddressSpaceByAreaID(areaID);

	if (fSpace == nullptr)
		return -ESRCH;

	fSpace->ReadLock();

	area = VMAreaHash::Lookup(areaID, fSpace);

	if (area == nullptr) {
		fSpace->ReadUnlock();
		fSpace->Put();
		fSpace = nullptr;
		return -EINVAL;
	}

	fLocked = true;
	return 0;
}

bool AddressSpaceReadLocker::Lock() {
	if (fLocked)
		return true;
	if (fSpace == nullptr)
		return false;

	fSpace->ReadLock();
	fLocked = true;

	return true;
}

void AddressSpaceReadLocker::Unlock() {
	if (fLocked) {
		fSpace->ReadUnlock();
		fLocked = false;
	}
}

//	#pragma mark - AddressSpaceWriteLocker

AddressSpaceWriteLocker::AddressSpaceWriteLocker(uint32_t team) :
		fSpace(nullptr), fLocked(false), fDegraded(false) {
	SetTo(team);
}

AddressSpaceWriteLocker::AddressSpaceWriteLocker(VMAddressSpace *space,
		bool getNewReference) :
		fSpace(nullptr), fLocked(false), fDegraded(false) {
	SetTo(space, getNewReference);
}

AddressSpaceWriteLocker::~AddressSpaceWriteLocker() {
	Unset();
}

void AddressSpaceWriteLocker::Unset() {
	Unlock();
	if (fSpace != nullptr) {
		fSpace->Put();
		fSpace = nullptr;
	}
}

int AddressSpaceWriteLocker::SetTo(uint32_t team) {
	Unset();

	fSpace = VMAddressSpace::Get(team);
	if (fSpace == nullptr)
		return -ESRCH;

	fSpace->WriteLock();

	fLocked = true;
	return 0;
}

void AddressSpaceWriteLocker::SetTo(VMAddressSpace *space,
		bool getNewReference) {
	Unset();

	fSpace = space;

	if (getNewReference)
		fSpace->Get();

	fSpace->WriteLock();
	fLocked = true;
}

int AddressSpaceWriteLocker::SetFromArea(int areaID, VMArea *&area) {
	Unset();

	fSpace = GetAddressSpaceByAreaID(areaID);
	if (fSpace == nullptr)
		return -EINVAL;

	fSpace->WriteLock();

	area = VMAreaHash::Lookup(areaID, fSpace);

	if (area == nullptr) {
		fSpace->WriteUnlock();
		fSpace->Put();
		fSpace = nullptr;
		return -EINVAL;
	}

	fLocked = true;
	return 0;
}

int AddressSpaceWriteLocker::SetFromArea(uint32_t team, int areaID,
		bool allowKernel, VMArea *&area) {
	Unset();

	VMAreaHash::ReadLock();

	area = VMAreaHash::LookupLocked(areaID);
	if (area != nullptr
			&& (area->address_space->ID() == (pid_t) team
					|| (allowKernel
							&& team == (uint32_t) VMAddressSpace::KernelID()))) {
		fSpace = area->address_space;
		fSpace->Get();
	}

	VMAreaHash::ReadUnlock();

	if (fSpace == nullptr)
		return -EINVAL;

	// Second try to get the area -- this time with the address space
	// write lock held

	fSpace->WriteLock();

	area = VMAreaHash::Lookup(areaID, fSpace);

	if (area == nullptr) {
		fSpace->WriteUnlock();
		fSpace->Put();
		fSpace = nullptr;
		return -EINVAL;
	}

	fLocked = true;
	return 0;
}

int AddressSpaceWriteLocker::SetFromArea(uint32_t team, int areaID,
		VMArea *&area) {
	return SetFromArea(team, areaID, false, area);
}

void AddressSpaceWriteLocker::Unlock() {
	if (fLocked) {
		if (fDegraded) {
			fSpace->ReadUnlock();
		} else {
			fSpace->WriteUnlock();
		}

		fLocked = false;
		fDegraded = false;
	}
}

void AddressSpaceWriteLocker::DegradeToReadLock() {
	ASSERT(!fDegraded);

	fSpace->ReadLock();
	fSpace->WriteUnlock();
	fDegraded = true;
}

//	#pragma mark - MultiAddressSpaceLocker

MultiAddressSpaceLocker::~MultiAddressSpaceLocker() {
	Unset();
	free(fItems);
}

bool MultiAddressSpaceLocker::_ResizeIfNeeded() {
	if (fCount == fCapacity) {
		lock_item *items = (lock_item*) realloc(fItems,
				(fCapacity + 4) * sizeof(lock_item));
		if (items == nullptr)
			return false;

		fCapacity += 4;
		fItems = items;
	}

	return true;
}

int32_t MultiAddressSpaceLocker::_IndexOfAddressSpace(
		VMAddressSpace *space) const {
	for (int32_t i = 0; i < fCount; i++) {
		if (fItems[i].space == space)
			return i;
	}

	return -1;
}

int MultiAddressSpaceLocker::_AddAddressSpace(VMAddressSpace *space,
		bool writeLock, VMAddressSpace **_space) {
	if (!space)
		return -EINVAL;

	int32_t index = _IndexOfAddressSpace(space);
	if (index < 0) {
		if (!_ResizeIfNeeded()) {
			space->Put();
			return -ENOMEM;
		}

		lock_item &item = fItems[fCount++];
		item.space = space;
		item.write_lock = writeLock;
	} else {

		// one reference is enough
		space->Put();

		fItems[index].write_lock |= writeLock;
	}

	if (_space != nullptr)
		*_space = space;

	return 0;
}

void MultiAddressSpaceLocker::Unset() {
	Unlock();

	for (int32_t i = 0; i < fCount; i++)
		fItems[i].space->Put();

	fCount = 0;
}

int MultiAddressSpaceLocker::Lock() {
	ASSERT(!fLocked);

	// Lock in descending order (kernel space last)
	std::sort(fItems, fItems + fCount,
			[](const lock_item &left, const lock_item &right) -> bool {
				return left.space->ID() > right.space->ID();
			});

	for (int32_t i = 0; i < fCount; i++) {
		int status;
		if (fItems[i].write_lock)
			status = fItems[i].space->WriteLock();
		else
			status = fItems[i].space->ReadLock();

		if (status < 0) {
			while (--i >= 0) {
				if (fItems[i].write_lock)
					fItems[i].space->WriteUnlock();
				else
					fItems[i].space->ReadUnlock();
			}
			return status;
		}
	}

	fLocked = true;
	return 0;
}

void MultiAddressSpaceLocker::Unlock() {
	if (!fLocked)
		return;

	for (int32_t i = 0; i < fCount; i++) {
		if (fItems[i].write_lock)
			fItems[i].space->WriteUnlock();
		else
			fItems[i].space->ReadUnlock();
	}

	fLocked = false;
}

/*!	Adds all address spaces of the areas associated with the given area's cache,
 locks them, and locks the cache (including a reference to it). It retries
 until the situation is stable (i.e. the neither cache nor cache's areas
 changed) or an error occurs.
 */
int MultiAddressSpaceLocker::AddAreaCacheAndLock(int areaID,
		bool writeLockThisOne, bool writeLockOthers, VMArea *&_area,
		VMCache **_cache) {
	// remember the original state
	int originalCount = fCount;
	lock_item *originalItems = nullptr;
	if (fCount > 0) {
		originalItems = new (std::nothrow) lock_item[fCount];
		if (originalItems == nullptr)
			return -ENOMEM;
		std::copy_n(fItems, fCount, originalItems);
	}
	ArrayDeleter<lock_item> _(originalItems);

	// get the cache
	VMCache *cache;
	VMArea *area;
	int error;
	{
		AddressSpaceReadLocker locker;
		error = locker.SetFromArea(areaID, area);
		if (error != 0)
			return error;

		cache = vm_area_get_locked_cache(area);
	}

	while (true) {
		// add all areas
		VMArea *firstArea = cache->areas;
		for (VMArea *currentArea = firstArea; currentArea; currentArea =
				currentArea->cache_next) {
			error = AddArea(currentArea,
					currentArea == area ? writeLockThisOne : writeLockOthers);
			if (error != 0) {
				vm_area_put_locked_cache(cache);
				return error;
			}
		}

		// unlock the cache and attempt to lock the address spaces
		vm_area_put_locked_cache(cache);

		error = Lock();
		if (error != 0)
			return error;

		// lock the cache again and check whether anything has changed

		// check whether the area is gone in the meantime
		area = VMAreaHash::Lookup(areaID);

		if (area == nullptr) {
			Unlock();
			return -EINVAL;
		}

		// lock the cache
		VMCache *oldCache = cache;
		cache = vm_area_get_locked_cache(area);

		// If neither the area's cache has changed nor its area list we're
		// done.
		if (cache == oldCache && firstArea == cache->areas) {
			_area = area;
			if (_cache != nullptr)
				*_cache = cache;
			return 0;
		}

		// Restore the original state and try again.

		// Unlock the address spaces, but keep the cache locked for the next
		// iteration.
		Unlock();

		// Get an additional reference to the original address spaces.
		for (int32_t i = 0; i < originalCount; i++)
			originalItems[i].space->Get();

		// Release all references to the current address spaces.
		for (int32_t i = 0; i < fCount; i++)
			fItems[i].space->Put();

		// Copy over the original state.
		fCount = originalCount;
		if (originalItems != nullptr) {
			std::copy_n(originalItems, fCount, fItems);
		}
	}
}
