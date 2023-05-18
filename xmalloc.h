
#ifndef __XMALLOC_H__
#define __XMALLOC_H__

#include <core/kconfig.h>
#include <core/types.h>
#include <core/cache.h>
#include <core/linkage.h>

#ifndef __ASSEMBLY__

#define CACHE_DONT_WAIT_FOR_MEMORY		0x001
#define CACHE_CLEAR_MEMORY				0x002
#define CACHE_DONT_LOCK_KERNEL			0x004
#define CACHE_USE_RESERVE 				0x008
#define CACHE_ALIGN_ON_SIZE				0x010
#define CACHE_LARGE_SLAB				0x020
#define CACHE_DURING_BOOT				0x040
#define CACHE_NO_DEPOT					0x080
#define CACHE_CONTIGUOUS_MEMORY			0x100
#define CACHE_DONT_OOM_KILL				0x200

#define CACHE_MIN_ALLOC_SIZE			16

typedef int (*object_cache_constructor)(void* cookie, void* object);
typedef void (*object_cache_destructor)(void* cookie, void* object);
typedef void (*object_cache_reclaimer)(void* cookie, int32_t level);

__BEGIN_DECLS

#define __XM_NONE      0x00 /* No specific requirements */
#define __XM_ZERO      0x01 /* Zeroed memory            */
#define __XM_POISON    0x02 /* Poisoned memmory         */

#define XM_NONE        __XM_NONE
#define XM_ZERO        __XM_ZERO
#define XM_POISON      __XM_POISON

/* Allocate space for typed object. */
#define xmalloc(_type) ((_type *)_xmalloc(sizeof(_type), __alignof__(_type), XM_NONE))
#define xzalloc(_type) ((_type *)_xmalloc(sizeof(_type), __alignof__(_type), XM_ZERO))

/* Allocate space for array of typed objects. */
#define xmalloc_array(_type, _num) ((_type *)_xmalloc_array(sizeof(_type), __alignof__(_type), _num, XM_NONE))
#define xzalloc_array(_type, _num) ((_type *)_xmalloc_array(sizeof(_type), __alignof__(_type), _num, XM_ZERO))

/* Allocate untyped storage. */
#define xmalloc_bytes(_bytes) (_xmalloc(_bytes, SMP_CACHE_BYTES, XM_NONE))
#define xzalloc_bytes(_bytes) (_xmalloc(_bytes, SMP_CACHE_BYTES, XM_ZERO))

void *_xmalloc(size_t size, size_t align, unsigned int flags) __alloc_size(1)
	__alloc_align(2) __result_use_check __malloc_like2(free, 1) __attribute__((deprecated("This function is deprecated. Use malloc/calloc or create_area")));
void *_xmalloc_array(size_t size, size_t align, size_t num, unsigned int flags)
	__alloc_size2(1, 3) __alloc_align(2) __result_use_check __malloc_like2(free, 1) __attribute__((deprecated("This function is deprecated. Use malloc/calloc or create_area")));

void xmalloc_init(void) __hidden;
void *xrealloc(void *old_ptr, size_t new_size)
	__result_use_check __alloc_size(2);

void free(void *addr);
void *malloc(size_t size)
	__malloc_like2(free, 1) __result_use_check __alloc_size(1);
void *realloc(void *addr, size_t size)
	__result_use_check __alloc_size(2);
void *calloc(size_t n_elements, size_t elem_size)
	__malloc_like2(free, 1) __result_use_check __alloc_size2(1, 2);

static inline void __attribute__((deprecated("This function is deprecated. Use free"))) xfree(const void * data) {
	free((void *)data);
}

void init_deferred_deleter(void) __hidden;

struct ObjectCache * create_object_cache(const char* name, size_t object_size, size_t alignment,
	void* cookie, object_cache_constructor constructor,
	object_cache_destructor destructor);
struct ObjectCache* create_object_cache_etc(const char* name, size_t objectSize, size_t alignment,
	size_t maximum, size_t magazineCapacity, size_t maxMagazineCount,
	uint32_t flags, void* cookie, object_cache_constructor constructor,
	object_cache_destructor destructor, object_cache_reclaimer reclaimer);
void delete_object_cache(struct ObjectCache * cache);
int object_cache_set_minimum_reserve(struct ObjectCache * cache, size_t objectCount);
void object_cache_free(struct ObjectCache* cache, void* object, uint32_t flags);
void* object_cache_alloc(struct ObjectCache * cache, uint32_t flags) __malloc_like __result_use_check;
int object_cache_reserve(struct ObjectCache* cache, size_t objectCount, uint32_t flags);

void object_cache_init_post_thread(void) __hidden;

#ifdef __cplusplus
struct backtrace_output;
void object_cache_print_debug(backtrace_output&) __hidden;
#endif

void block_allocator_init_boot(void) __hidden;

void* realloc_etc(void* oldBuffer, size_t newSize, int flags)
	__result_use_check __alloc_size(2);
void* malloc_etc(size_t size, uint32_t flags)
	__malloc_like2(free, 1) __result_use_check __alloc_size(1);
void* memalign(size_t alignment, size_t size)
	__malloc_like2(free, 1) __result_use_check __alloc_size(2) __alloc_align(1);
void * memalign_etc(size_t alignment, size_t size, uint32_t flags)
	__malloc_like2(free, 1) __result_use_check __alloc_size(2) __alloc_align(1);
int posix_memalign(void** _pointer, size_t alignment, size_t size)
	__result_use_check;
void free_etc(void *address, uint32_t flags);

void vfree(void * pointer);
void * vmalloc_etc(size_t length, int priority, int wait_flags)
	__malloc_like2(vfree, 1) __alloc_size(1) __result_use_check __assume_aligned(PAGE_SIZE);
void * vmalloc(size_t length) __alloc_size(1)
	__malloc_like2(vfree, 1) __alloc_size(1) __result_use_check __assume_aligned(PAGE_SIZE);
void * vzalloc(size_t length) __alloc_size(1)
	__malloc_like2(vfree, 1) __alloc_size(1) __result_use_check __assume_aligned(PAGE_SIZE);

__END_DECLS

#ifdef __cplusplus
struct ObjectCache;

void* operator new(size_t size, ObjectCache *objectCache,
		uint32_t flags) noexcept
		__attribute__((__externally_visible__, __alloc_size__(1), __malloc__));

template<typename Type>
inline void
object_cache_delete(ObjectCache* objectCache, Type* object, uint32_t flags = 0)
{
	if (object != nullptr) {
		object->~Type();
		object_cache_free(objectCache, object, flags);
	}
}
#endif

#ifdef __cplusplus
struct wait_flags_t {
	int			fFlags = 0;

	explicit wait_flags_t(int flags = 0) :
		fFlags(flags)
	{
	}

	wait_flags_t& operator |= (int other) {
		fFlags |= other;
		return *this;
	}

	wait_flags_t operator | (int other) const {
		return wait_flags_t(fFlags | other);
	}
};
#endif

#endif

#endif
