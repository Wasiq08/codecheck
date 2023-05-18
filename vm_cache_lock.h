#ifndef _MM_VM_CACHE_LOCK_H_
#define _MM_VM_CACHE_LOCK_H_

#include <core/smp.h>
#include <core/mutex.h>
#include <lib/AutoLock.h>

struct VMCacheLock {
	constexpr VMCacheLock() noexcept = default;

	VMCacheLock(const VMCacheLock &) = delete;
	VMCacheLock &operator=(const VMCacheLock &) = delete;

	void Destroy() noexcept;
	int Lock() noexcept;
	void Unlock() noexcept;
	bool TryLock() noexcept;
	int SwitchFromLock(VMCacheLock &fromLock) noexcept;
	int SwitchFromReadLock(rw_lock_t &readLock) noexcept;
	void AssertLocked() noexcept;

private:
	int _LockInternal(InterruptsSmpSpinLocker *locker) noexcept;
	int _LockThreadsLocked(InterruptsSmpSpinLocker *locker) noexcept;
	void _UnlockEtc(InterruptsSmpSpinLocker *locker) noexcept;

private:
	struct _Waiter;
	std::atomic<int32_t> count{0};
	_Waiter *waiters{nullptr};
	uintptr_t holder{0};
	bool released{false};
};

#endif /* _MM_VM_CACHE_LOCK_H_ */
