#include <mm/vm_cache_lock.h>
#include <core/thread.h>
#include <core/cpu.h>
#include <lib/AutoLock.h>

struct vmcache_lock_chain {
	smp_spinlock		lock{"VMCache Chain"};
};

#define VMC_TABLESIZE 32
static_assert(powerof2(VMC_TABLESIZE));
#define VMC_MASK     (VMC_TABLESIZE - 1)
#define VMC_SHIFT    8
#define VMC_HASH(wc) ((((uintptr_t) (wc) >> VMC_SHIFT) ^ (uintptr_t) (wc)) & \
	                  VMC_MASK)
#define VMC_LOOKUP(wc) vmc_chains[VMC_HASH(wc)].lock

static vmcache_lock_chain vmc_chains[VMC_TABLESIZE] alignas(CACHE_LINE_SIZE);

struct VMCacheLock::_Waiter {
	struct thread *thread = get_current_thread();
	_Waiter *next         = nullptr;// next in queue
	_Waiter *last         = nullptr;// last in queue (valid for the first in queue)
};

void VMCacheLock::Destroy() noexcept {
	// unblock all waiters
	InterruptsSmpSpinLocker locker(VMC_LOOKUP(this));

	while (auto *waiter = waiters) {
		// dequeue
		waiters = waiter->next;

		struct thread *thread = waiter->thread;
		waiter->thread        = nullptr;

		thread_unblock(thread, -EINVAL, THREAD_WAIT_VMCACHE);
	}

	holder   = 0;
	released = false;
	count.store(0, std::memory_order_relaxed);
}

int VMCacheLock::_LockThreadsLocked(InterruptsSmpSpinLocker *locker) noexcept {
	KASSERT(holder != (uintptr_t) get_current_thread(),
	        ("%s: VMCacheLock %p already owned by current thread", __func__, this));

	if (count.fetch_sub(1, std::memory_order_acquire) < 0) {
		return _LockInternal(locker);
	}

	holder = (uintptr_t) get_current_thread();
	return 0;
}

int VMCacheLock::_LockInternal(InterruptsSmpSpinLocker *locker) noexcept {
#if defined(CONFIG_DEBUG)
	// Locker will make interrupts disabled
	if (!locker && !are_interrupts_enabled() && !gKernelStartup) {
		if (get_preempt_count()) {
			panic("_mutex_lock(): called with preemption disabled for lock %p", this);
		} else {
			static check_preempt_helper checker;
			checker.warn_once(__func__, __builtin_return_address(0));
		}
	}
#endif

	KASSERT(holder != (uintptr_t) get_current_thread(), ("%s: VMCacheLock %p double lock detected", __func__, this));

	// lock only, if !lockLocked
	InterruptsSmpSpinLocker lockLocker;
	if (locker == nullptr) {
		lockLocker.SetTo(VMC_LOOKUP(this), false);
		locker = &lockLocker;
	}

	// Might have been released after we decremented the count, but before
	// we acquired the spinlock.
	if (std::exchange(released, false)) {
		holder = (uintptr_t) get_current_thread();
		return 0;
	}

	// enqueue in waiter list
	_Waiter waiter{};

	waiter.next = nullptr;

	if (waiters != nullptr) {
		waiters->last->next = &waiter;
	} else {
		waiters = &waiter;
	}

	waiters->last = &waiter;

	// block
	thread_block_prepare(0, THREAD_WAIT_VMCACHE, this);
	locker->Unlock();

	int error = thread_block();

	if (error == 0) {
		KASSERT(holder == (uintptr_t) get_current_thread(), ("%s: VMCacheLock %p not owned by us", __func__, this));
	}

	return error;
}

int VMCacheLock::Lock() noexcept {
	if (count.fetch_sub(1, std::memory_order_acquire) < 0) {
		return _LockInternal(nullptr);
	}

	holder = (uintptr_t) get_current_thread();
	return 0;
}

void VMCacheLock::Unlock() noexcept {
	_UnlockEtc(nullptr);
}

void VMCacheLock::_UnlockEtc(InterruptsSmpSpinLocker *parentLocker) noexcept {
#if defined(CONFIG_DEBUG)
	if (__predict_false(!gKernelStartup)) {
		KASSERT(holder == (uintptr_t) get_current_thread(), ("%p: VMCacheLock not owned by current thread", this));
	}
#endif

	holder = 0;

	if (count.fetch_add(1, std::memory_order_release) < -1) {
		InterruptsSmpSpinLocker locker(VMC_LOOKUP(this), false, false);

		if (parentLocker == nullptr || locker.GetLockable() != parentLocker->GetLockable()) {
			locker.Lock();
		}

		auto *waiter = waiters;
		if (waiter != nullptr) {
			// dequeue the first waiter
			waiters = waiter->next;
			if (waiters != nullptr) {
				waiters->last = waiter->last;
			}

			holder = (uintptr_t) waiter->thread;

			// unblock thread
			thread_unblock(waiter->thread, 0, THREAD_WAIT_VMCACHE);
		} else {
			// There are no waiters, so mark the lock as released.
			released = true;
		}
	}
}

bool VMCacheLock::TryLock() noexcept {
	int32_t expected = 0;

	if (!count.compare_exchange_strong(expected, -1, std::memory_order_acquire)) {
		return false;
	}

	holder = (uintptr_t) get_current_thread();
	return true;
}

int VMCacheLock::SwitchFromLock(VMCacheLock &fromLock) noexcept {
#if defined(CONFIG_DEBUG)
	if (!are_interrupts_enabled() && !gKernelStartup) {
		if (get_preempt_count()) {
			panic("%s: called with preemption disabled "
			      "for locks %p, %p",
			      __func__, this, &fromLock);
		} else {
			static check_preempt_helper checker;
			checker.warn_once(__func__, __builtin_return_address(0));
		}
	}
#endif

	KASSERT(fromLock.holder == (uintptr_t) get_current_thread(),
	        ("%s: VMCacheLock %p not owned by us", __func__, &fromLock));
	KASSERT(this->holder != (uintptr_t) get_current_thread(),
	        ("%s: VMCacheLock %p already owned by us", __func__, this));

	InterruptsSmpSpinLocker locker(VMC_LOOKUP(this));

	fromLock._UnlockEtc(&locker);

	return _LockThreadsLocked(&locker);
}

int VMCacheLock::SwitchFromReadLock(rw_lock_t &readLock) noexcept {
#if defined(CONFIG_DEBUG)
	if (!are_interrupts_enabled() && !gKernelStartup) {
		if (get_preempt_count()) {
			panic("%s: called with preemption disabled "
			      "for locks %p, %p",
			      __func__, &readLock, this);
		} else {
			static check_preempt_helper checker;
			checker.warn_once(__func__, __builtin_return_address(0));
		}
	}
#endif

	KASSERT(holder != (uintptr_t) get_current_thread(), ("%s: VMCacheLock %p already owned by us", __func__, this));

	InterruptsSmpSpinLocker locker(VMC_LOOKUP(this));

	readLock.unlock_shared();

	return _LockThreadsLocked(&locker);
}

void VMCacheLock::AssertLocked() noexcept {
	KASSERT(holder == (uintptr_t) get_current_thread(), ("%s: VMCacheLock %p not owned by us", __func__, this));
}
