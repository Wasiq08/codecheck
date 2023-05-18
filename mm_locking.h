
#ifndef KERNEL_CORE_MM_LOCKING_H_
#define KERNEL_CORE_MM_LOCKING_H_


#include <core/kconfig.h>
#include <mm/VMAddressSpace.h>

struct VMAddressSpace;
struct VMArea;
struct VMCache;

class AddressSpaceLockerBase {
public:
	constexpr AddressSpaceLockerBase() = default;
	AddressSpaceLockerBase(const AddressSpaceLockerBase&) = delete;
	AddressSpaceLockerBase& operator =(const AddressSpaceLockerBase&) = delete;

	static VMAddressSpace* GetAddressSpaceByAreaID(int id);
};

class AddressSpaceReadLocker: private AddressSpaceLockerBase {
public:
	AddressSpaceReadLocker(uint32_t team);
	AddressSpaceReadLocker(VMAddressSpace *space, bool getNewReference);
	constexpr AddressSpaceReadLocker() = default;
	~AddressSpaceReadLocker();

	int SetTo(uint32_t team);
	void SetTo(VMAddressSpace *space, bool getNewReference);
	int SetFromArea(int areaID, VMArea *&area);

	[[nodiscard]] bool IsLocked() const {
		return fLocked;
	}
	bool Lock();
	void Unlock();

	void Unset();

	[[nodiscard]] VMAddressSpace* AddressSpace() const {
		return fSpace;
	}

private:
	VMAddressSpace *fSpace { nullptr };
	bool fLocked { false };
};

class AddressSpaceWriteLocker: private AddressSpaceLockerBase {
public:
	AddressSpaceWriteLocker(uint32_t team);
	AddressSpaceWriteLocker(VMAddressSpace *space, bool getNewReference);
	constexpr AddressSpaceWriteLocker() = default;
	~AddressSpaceWriteLocker();

	int SetTo(uint32_t team);
	void SetTo(VMAddressSpace *space, bool getNewReference);
	int SetFromArea(int areaID, VMArea *&area);
	int SetFromArea(uint32_t team, int areaID, bool allowKernel, VMArea *&area);
	int SetFromArea(uint32_t team, int areaID, VMArea *&area);

	[[nodiscard]] bool IsLocked() const {
		return fLocked;
	}
	void Unlock();

	void DegradeToReadLock();
	void Unset();

	[[nodiscard]] VMAddressSpace* AddressSpace() const {
		return fSpace;
	}

private:
	VMAddressSpace *fSpace { nullptr };
	bool fLocked { false };
	bool fDegraded { false };
};

class MultiAddressSpaceLocker: private AddressSpaceLockerBase {
public:
	constexpr MultiAddressSpaceLocker() = default;
	~MultiAddressSpaceLocker();

	int AddTeam(uint32_t team, bool writeLock,
			VMAddressSpace **_space = nullptr);
	int AddArea(int area, bool writeLock, VMAddressSpace **_space = nullptr);
	int AddArea(VMArea *area, bool writeLock,
			VMAddressSpace **_space = nullptr);

	int AddAreaCacheAndLock(int areaID, bool writeLockThisOne,
			bool writeLockOthers, VMArea *&_area, VMCache **_cache);

	int Lock();
	void Unlock();
	[[nodiscard]] bool IsLocked() const {
		return fLocked;
	}

	void Unset();

private:
	struct lock_item {
		VMAddressSpace *space;
		bool write_lock;
	};

	bool _ResizeIfNeeded();
	int32_t _IndexOfAddressSpace(VMAddressSpace *space) const;
	int _AddAddressSpace(VMAddressSpace *space, bool writeLock,
			VMAddressSpace **_space);

	lock_item *fItems { nullptr };
	int32_t fCapacity { 0 };
	int32_t fCount { 0 };
	bool fLocked { false };
};

inline int MultiAddressSpaceLocker::AddTeam(uint32_t team, bool writeLock,
		VMAddressSpace **_space) {
	return _AddAddressSpace(VMAddressSpace::Get(team), writeLock, _space);
}

inline int MultiAddressSpaceLocker::AddArea(int area, bool writeLock,
		VMAddressSpace **_space) {
	return _AddAddressSpace(GetAddressSpaceByAreaID(area), writeLock, _space);
}

inline int MultiAddressSpaceLocker::AddArea(VMArea *area, bool writeLock,
		VMAddressSpace **_space) {
	area->address_space->Get();
	return _AddAddressSpace(area->address_space, writeLock, _space);
}

#endif /* KERNEL_CORE_MM_LOCKING_H_ */
