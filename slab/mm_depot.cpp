
#include <mm/mm_depot.h>
#include <mm/mm_allocator.h>
#include <core/machine_desc.h>
#include <cinttypes>
#include <debug/debugger.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>

struct DepotMagazine {
			DepotMagazine*		next;
			uint16_t			current_round;
			uint16_t			round_count;
			void*				rounds[0];

public:
	[[nodiscard]] inline	bool				IsEmpty() const;
	[[nodiscard]] inline	bool				IsFull() const;

	inline	void*				Pop();
	inline	bool				Push(void* object);

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
			bool				ContainsObject(void* object) const;
#endif
};


struct depot_cpu_store {
	DepotMagazine*	loaded;
	DepotMagazine*	previous;
};


bool
DepotMagazine::IsEmpty() const
{
	return current_round == 0;
}


bool
DepotMagazine::IsFull() const
{
	return current_round == round_count;
}


void*
DepotMagazine::Pop()
{
	return rounds[--current_round];
}


bool
DepotMagazine::Push(void* object)
{
	if (IsFull())
		return false;

	rounds[current_round++] = object;
	return true;
}


#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS

bool
DepotMagazine::ContainsObject(void* object) const
{
	for (uint16_t i = 0; i < current_round; i++) {
		if (rounds[i] == object)
			return true;
	}

	return false;
}

#endif // PARANOID_KERNEL_FREE



static DepotMagazine*
alloc_magazine(object_depot* depot, uint32_t flags)
{
	DepotMagazine* magazine = (DepotMagazine*)slab_internal_alloc(
		sizeof(DepotMagazine) + depot->magazine_capacity * sizeof(void*),
		flags);
	if (magazine) {
		magazine->next = nullptr;
		magazine->current_round = 0;
		magazine->round_count = depot->magazine_capacity;
	}

	return magazine;
}


static void
free_magazine(DepotMagazine* magazine, uint32_t flags)
{
	slab_internal_free(magazine, flags);
}


static void
empty_magazine(object_depot* depot, DepotMagazine* magazine, uint32_t flags)
{
	for (uint16_t i = 0; i < magazine->current_round; i++)
		depot->return_object(depot, depot->cookie, magazine->rounds[i], flags);
	free_magazine(magazine, flags);
}


static bool
exchange_with_full(object_depot* depot, DepotMagazine*& magazine)
{
	ASSERT(magazine->IsEmpty());

	SmpSpinLocker _(depot->inner_lock);

	if (depot->full == nullptr)
		return false;

	depot->full_count--;
	depot->empty_count++;

	MemoryManager::_push(depot->empty, magazine);
	magazine = MemoryManager::_pop(depot->full);
	return true;
}


static bool
exchange_with_empty(object_depot* depot, DepotMagazine*& magazine,
	DepotMagazine*& freeMagazine)
{
	ASSERT(magazine == nullptr || magazine->IsFull());

	SmpSpinLocker _(depot->inner_lock);

	if (depot->empty == nullptr)
		return false;

	depot->empty_count--;

	if (magazine != nullptr) {
		if (depot->full_count < depot->max_count) {
			MemoryManager::_push(depot->full, magazine);
			depot->full_count++;
			freeMagazine = nullptr;
		} else
			freeMagazine = magazine;
	}

	magazine = MemoryManager::_pop(depot->empty);
	return true;
}


static void
push_empty_magazine(object_depot* depot, DepotMagazine* magazine)
{
	SmpSpinLocker _(depot->inner_lock);

	MemoryManager::_push(depot->empty, magazine);
	depot->empty_count++;
}


static inline depot_cpu_store*
object_depot_cpu(object_depot* depot)
{
	return &depot->stores[smp_get_current_cpu()];
}


// #pragma mark - public API


int
object_depot_init(object_depot* depot, size_t capacity, size_t maxCount,
	uint32_t flags, void* cookie, void (*return_object)(object_depot* depot,
		void* cookie, void* object, uint32_t flags))
{
	depot->full = nullptr;
	depot->empty = nullptr;
	depot->full_count = depot->empty_count = 0;
	depot->max_count = maxCount;
	depot->magazine_capacity = capacity;

	int cpuCount = smp_get_num_cpus();
	depot->stores = (depot_cpu_store*)slab_internal_alloc(
		sizeof(depot_cpu_store) * cpuCount, flags);
	if (depot->stores == nullptr) {
		return -ENOMEM;
	}

	for (int i = 0; i < cpuCount; i++) {
		depot->stores[i].loaded = nullptr;
		depot->stores[i].previous = nullptr;
	}

	depot->cookie = cookie;
	depot->return_object = return_object;

	return 0;
}


void
object_depot_destroy(object_depot* depot, uint32_t flags)
{
	object_depot_make_empty(depot, flags);

	slab_internal_free(depot->stores, flags);
	depot->stores = nullptr;
}


void*
object_depot_obtain(object_depot* depot)
{
	ReadLocker readLocker(depot->outer_lock);
	InterruptsLocker interruptsLocker;

	depot_cpu_store* store = object_depot_cpu(depot);

	// To better understand both the Alloc() and Free() logic refer to
	// Bonwick's ``Magazines and Vmem'' [in 2001 USENIX proceedings]

	// In a nutshell, we try to get an object from the loaded magazine
	// if it's not empty, or from the previous magazine if it's full
	// and finally from the Slab if the magazine depot has no full magazines.

	if (store->loaded == nullptr)
		return nullptr;

	while (true) {
		if (!store->loaded->IsEmpty())
			return store->loaded->Pop();

		if (store->previous
			&& (store->previous->IsFull()
				|| exchange_with_full(depot, store->previous))) {
			std::swap(store->previous, store->loaded);
		} else
			return nullptr;
	}
}


void
object_depot_store(object_depot* depot, void* object, uint32_t flags)
{
	ReadLocker readLocker(depot->outer_lock);
	InterruptsLocker interruptsLocker;

	depot_cpu_store* store = object_depot_cpu(depot);

	// We try to add the object to the loaded magazine if we have one
	// and it's not full, or to the previous one if it is empty. If
	// the magazine depot doesn't provide us with a new empty magazine
	// we return the object directly to the slab.

	while (true) {
		if (store->loaded != nullptr && store->loaded->Push(object))
			return;

		DepotMagazine* freeMagazine = nullptr;
		if ((store->previous != nullptr && store->previous->IsEmpty())
			|| exchange_with_empty(depot, store->previous, freeMagazine)) {
			std::swap(store->loaded, store->previous);

			if (freeMagazine != nullptr) {
				// Free the magazine that didn't have space in the list
				interruptsLocker.Unlock();
				readLocker.Unlock();

				empty_magazine(depot, freeMagazine, flags);

				readLocker.Lock();
				interruptsLocker.Lock();

				store = object_depot_cpu(depot);
			}
		} else {
			// allocate a new empty magazine
			interruptsLocker.Unlock();
			readLocker.Unlock();

			DepotMagazine* magazine = alloc_magazine(depot, flags);
			if (magazine == nullptr) {
				depot->return_object(depot, depot->cookie, object, flags);
				return;
			}

			readLocker.Lock();
			interruptsLocker.Lock();

			push_empty_magazine(depot, magazine);
			store = object_depot_cpu(depot);
		}
	}
}


void
object_depot_make_empty(object_depot* depot, uint32_t flags)
{
	WriteLocker writeLocker(depot->outer_lock);

	// collect the store magazines

	DepotMagazine* storeMagazines = nullptr;

	int cpuCount = smp_get_num_cpus();
	for (int i = 0; i < cpuCount; i++) {
		depot_cpu_store& store = depot->stores[i];

		if (store.loaded) {
			MemoryManager::_push(storeMagazines, store.loaded);
			store.loaded = nullptr;
		}

		if (store.previous) {
			MemoryManager::_push(storeMagazines, store.previous);
			store.previous = nullptr;
		}
	}

	// detach the depot's full and empty magazines

	DepotMagazine* fullMagazines = depot->full;
	depot->full = nullptr;

	DepotMagazine* emptyMagazines = depot->empty;
	depot->empty = nullptr;

	writeLocker.Unlock();

	// free all magazines

	while (storeMagazines != nullptr)
		empty_magazine(depot, MemoryManager::_pop(storeMagazines), flags);

	while (fullMagazines != nullptr)
		empty_magazine(depot, MemoryManager::_pop(fullMagazines), flags);

	while (emptyMagazines)
		free_magazine(MemoryManager::_pop(emptyMagazines), flags);
}


#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS

bool
object_depot_contains_object(object_depot* depot, void* object)
{
	WriteLocker writeLocker(depot->outer_lock);

	int cpuCount = smp_get_num_cpus();
	for (int i = 0; i < cpuCount; i++) {
		depot_cpu_store& store = depot->stores[i];

		if (store.loaded != nullptr && !store.loaded->IsEmpty()) {
			if (store.loaded->ContainsObject(object))
				return true;
		}

		if (store.previous != nullptr && !store.previous->IsEmpty()) {
			if (store.previous->ContainsObject(object))
				return true;
		}
	}

	for (DepotMagazine* magazine = depot->full; magazine != nullptr;
			magazine = magazine->next) {
		if (magazine->ContainsObject(object))
			return true;
	}

	return false;
}

#endif // PARANOID_KERNEL_FREE

#if defined(CONFIG_KDEBUGGER)

void
dump_object_depot(object_depot* depot)
{
	kprintf("  full:     %p, count %zu\n", depot->full, depot->full_count);
	kprintf("  empty:    %p, count %zu\n", depot->empty, depot->empty_count);
	kprintf("  max full: %zu\n", depot->max_count);
	kprintf("  capacity: %zu\n", depot->magazine_capacity);
	kprintf("  stores:\n");

	int cpuCount = smp_get_num_cpus();

	for (int i = 0; i < cpuCount; i++) {
		kprintf("  [%d] loaded:   %p\n", i, depot->stores[i].loaded);
		kprintf("      previous: %p\n", depot->stores[i].previous);
	}
}


int
dump_object_depot(int argCount, char** args)
{
	if (argCount != 2)
		kprintf("usage: %s [address]\n", args[0]);
	else
		dump_object_depot((object_depot*)parse_expression(args[1]));

	return 0;
}


int
dump_depot_magazine(int argCount, char** args)
{
	if (argCount != 2) {
		kprintf("usage: %s [address]\n", args[0]);
		return 0;
	}

	DepotMagazine* magazine = (DepotMagazine*)parse_expression(args[1]);

	kprintf("next:          %p\n", magazine->next);
	kprintf("current_round: %" PRIu16 "\n", magazine->current_round);
	kprintf("round_count:   %" PRIu16 "\n", magazine->round_count);

	for (uint16_t i = 0; i < magazine->current_round; i++)
		kprintf("  [%i] %p\n", i, magazine->rounds[i]);

	return 0;
}


#endif
