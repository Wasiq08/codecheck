
#include <core/compiler.h>
#include <core/debug.h>
#include <mm/mm.h>
#include <mm/xmalloc.h>
#include <core/module.h>
#include <core/thread.h>
#include <core/condvar.h>
#include <lib/string.h>
#include <lib/deferred_deletable.h>
#include <lib/AutoLock.h>
#include <mm/mm_allocator.h>

static struct deferred_deletable * deferred_head = nullptr;
static struct deferred_deletable * deferred_tail = nullptr;
static void * deferred_free_queue = nullptr;
static smp_spinlock deferred_deleter_lock{"deferred deleter"};
static cond_var_t deferred_deleter_cond{"deferred deleter"};

void *_xmalloc(size_t size, size_t align, unsigned int flags)
{
	(void)align;
	return malloc_etc(size,
			((flags & XM_ZERO) ? CACHE_CLEAR_MEMORY : 0)
					| CACHE_DONT_WAIT_FOR_MEMORY | CACHE_CONTIGUOUS_MEMORY);
}

void *_xmalloc_array(size_t size, size_t align, size_t num, unsigned int flags)
{
	(void)align;
	return memalign_etc(align, size * num,
			((flags & XM_ZERO) ? CACHE_CLEAR_MEMORY : 0)
					| CACHE_DONT_WAIT_FOR_MEMORY | CACHE_CONTIGUOUS_MEMORY);
}

void *xrealloc(void *old_ptr, size_t new_size)
{
	return realloc_etc(old_ptr, new_size,
			CACHE_DONT_WAIT_FOR_MEMORY | CACHE_CONTIGUOUS_MEMORY);
}

void delete_deferred(struct deferred_deletable * obj)
{
	InterruptsSmpSpinLocker locker(deferred_deleter_lock);
	obj->next_link = nullptr;
	if (deferred_tail) {
		deferred_tail->next_link = obj;
	} else {
		deferred_head = obj;
	}
	deferred_tail = obj;
	if (obj == deferred_head) {
		deferred_deleter_cond.NotifyOne();
	}
}

void free_deferred(void * obj)
{
	if(obj) {
		InterruptsSmpSpinLocker locker(deferred_deleter_lock);
		if(!deferred_free_queue)
			deferred_deleter_cond.NotifyOne();
		*(void **)obj = deferred_free_queue;
		deferred_free_queue = obj;
	}
}

static int deleter_worker(void *unused) {
	(void) unused;
	struct deferred_deletable *item, *next;
	void *dfc_free, *dfc_free_next;

	for (;;) {
		InterruptsSmpSpinLocker locker(deferred_deleter_lock);
		if (!deferred_head && !deferred_free_queue) {
			cond_var_entry entry;
			deferred_deleter_cond.Add(&entry);
			locker.Unlock();
			entry.Wait();
			continue;
		}

		item = deferred_head;
		dfc_free = deferred_free_queue;
		deferred_head = nullptr;
		deferred_tail = nullptr;
		deferred_free_queue = nullptr;
		locker.Unlock();

		while (item) {
			next = item->next_link;
			item->handler(item);
			item = next;
		}

		while (dfc_free) {
			dfc_free_next = *(void**) dfc_free;
			free(dfc_free);
			dfc_free = dfc_free_next;
		}
	}

	return 0;
}

void __hidden __init_text init_deferred_deleter(void)
{
	tid_t id = spawn_kernel_thread_etc(deleter_worker, "deleter worker",
			K_LOW_PRIORITY, nullptr, 0);
	if(id < 0)
		panic("Can't create deferred deleter thread");
	resume_thread(id);
}

__warn_references(xfree, "This API is deprecated. Use free()");
__warn_references(_xmalloc, "This API is deprecated. Use malloc or create_area");
__warn_references(_xmalloc_array, "This API is deprecated. Use malloc or create_area");
__warn_references(xrealloc, "This API is deprecated. Use realloc or resize_area");

void *
operator new([[maybe_unused]] size_t size, ObjectCache* objectCache, uint32_t flags) noexcept
{
	KASSERT(objectCache != nullptr,
			("%s: Object cache pointer is NULL", __func__));
	KASSERT(size <= objectCache->object_size,
			("%s: Allocation %zu exceeds cache size %zu",
			__func__, size, objectCache->object_size));
	return object_cache_alloc(objectCache, flags);
}
