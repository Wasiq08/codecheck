

#ifndef _KERNEL_MM_MM_DEPOT_H_
#define _KERNEL_MM_MM_DEPOT_H_

#include <core/mutex.h>

struct DepotMagazine;

struct object_depot {
	rw_lock_t				outer_lock{"object depot"};
	smp_spinlock			inner_lock{"object depot inner"};
	DepotMagazine*			full{};
	DepotMagazine*			empty{};
	size_t					full_count{};
	size_t					empty_count{};
	size_t					max_count{};
	size_t					magazine_capacity{};
	struct depot_cpu_store*	stores{};
	void*					cookie{};

	void (*return_object)(struct object_depot* depot, void* cookie,
		void* object, uint32_t flags) = nullptr;

	object_depot() noexcept = default;
	object_depot& operator = (const object_depot&) = delete;
	object_depot(const object_depot&) = delete;
};

#ifdef __cplusplus
extern "C" {
#endif

int object_depot_init(object_depot* depot, size_t capacity,
	size_t maxCount, uint32_t flags, void* cookie,
	void (*returnObject)(object_depot* depot, void* cookie, void* object,
			uint32_t flags));
void object_depot_destroy(object_depot* depot, uint32_t flags);

void* object_depot_obtain(object_depot* depot);
void object_depot_store(object_depot* depot, void* object, uint32_t flags);

void object_depot_make_empty(object_depot* depot, uint32_t flags);

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
bool object_depot_contains_object(object_depot* depot, void* object);
#endif

#ifdef __cplusplus
}
#endif


#endif /* _KERNEL_MM_MM_DEPOT_H_ */
