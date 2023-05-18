
#include <core/debug.h>
#include <core/machine_desc.h>
#include <mm/mm_allocator.h>
#include <mm/mm_resource.h>
#include <lib/cxxabi.h>
#include <lib/AutoLock.h>
#include <cinttypes>
#include <debug/debug_heap.h>
#include <debug/unwind.h>

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
static const uint32_t _fill_block_pattern = 0xcccccccc;

static inline void* fill_block(void* buffer, size_t size, uint32_t pattern)
{
	if (buffer == nullptr)
		return nullptr;

	size &= ~(sizeof(pattern) - 1);
	for (size_t i = 0; i < size / sizeof(pattern); i++)
		((uint32_t*)buffer)[i] = pattern;

	return buffer;
}
#endif

static inline void* fill_allocated_block(void* buffer, [[maybe_unused]] size_t size)
{
#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	return fill_block(buffer, size, _fill_block_pattern);
#else
	return buffer;
#endif
}

static inline void*
fill_freed_block(void* buffer, [[maybe_unused]] size_t size)
{
#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	return fill_block(buffer, size, 0xdeadbeef);
#else
	return buffer;
#endif
}

static const size_t kBlockSizes[] = {
	16, 24, 32, 48, 64, 80, 96, 112,
	128, 160, 192, 224, 256, 320, 384, 448,
	512, 640, 768, 896, 1024, 1280, 1536, 1792,
	2048, 2560, 3072, 3584, 4096, 4608, 5120, 5632,
	6144, 6656, 7168, 7680, 8192,
	0
};

static const size_t kNumBlockSizes = sizeof(kBlockSizes) / sizeof(size_t) - 1;

static ObjectCache * sBlockCaches[kNumBlockSizes];

static uintptr_t sBootStrapMemory = 0;
static size_t sBootStrapMemorySize = 0;
static size_t sUsedBootStrapMemory = 0;

namespace {
STAILQ_HEAD(ObjectCacheList, ObjectCache);
}

static ObjectCacheList sObjectCaches = STAILQ_HEAD_INITIALIZER(sObjectCaches);
static ObjectCacheList sMaintenanceQueue = STAILQ_HEAD_INITIALIZER(sMaintenanceQueue);
static DEFINE_MUTEX_ETC(sObjectCacheListLock, "object cache list");
static DEFINE_MUTEX_ETC(sMaintenanceLock, "object cache maintenance");

static inline slab *
slab_in_pages(const void *pages, size_t slab_size)
{
	return (slab *)(((uint8_t *)pages) + slab_size - sizeof(slab));
}

static void
increase_object_reserve(ObjectCache* cache)
{
	MutexLocker locker(sMaintenanceLock);

	cache->maintenance_resize = true;

	if (!cache->maintenance_pending) {
		cache->maintenance_pending = true;
		STAILQ_INSERT_TAIL(&sMaintenanceQueue, cache, maintenance_link);
		MemoryManager::sMaintenanceCond.NotifyAll();
	}
}

static void
object_cache_return_object_wrapper([[maybe_unused]] object_depot* depot, void* cookie,
	void* object, uint32_t flags)
{
	ObjectCache* cache = (ObjectCache*)cookie;

	MutexLocker _(cache->lock);
	cache->ReturnObjectToSlab(cache->ObjectSlab(object), object, flags);
}

int ObjectCache::Init(const char*name_, size_t objectSize, size_t alignment_,
	size_t maximum_, size_t magazineCapacity, size_t maxMagazineCount,
	uint32_t flags_, void*cookie_, object_cache_constructor constructor_,
	object_cache_destructor destructor_, object_cache_reclaimer reclaimer_)
{
	strlcpy(this->name, name_, sizeof(this->name));

	if (objectSize < sizeof(object_link))
		objectSize = sizeof(object_link);

	if (alignment_ < kMinObjectAlignment)
		alignment_ = kMinObjectAlignment;

	if (alignment_ > 0 && (objectSize & (alignment_ - 1)))
		object_size = objectSize + alignment_ - (objectSize & (alignment_ - 1));
	else
		object_size = objectSize;

	this->alignment = alignment_;
	cache_color_cycle = 0;
	total_objects = 0;
	used_count = 0;
	empty_count = 0;
	pressure = 0;
	min_object_reserve = 0;

	maintenance_pending = false;
	maintenance_in_progress = false;
	maintenance_resize = false;
	maintenance_delete = false;

	usage = 0;
	this->maximum = maximum_;

	this->flags = flags_;

	resize_request = nullptr;
	resize_entry_can_wait = nullptr;
	resize_entry_dont_wait = nullptr;

	if(smp_get_num_cpus() == 1)
		this->flags |= CACHE_NO_DEPOT;

	if (!(this->flags & CACHE_NO_DEPOT)) {
		// Determine usable magazine configuration values if none had been given
		if (magazineCapacity == 0) {
			magazineCapacity =
					objectSize < 256 ? 32 : (objectSize < 512 ? 16 : 8);
		}
		if (maxMagazineCount == 0)
			maxMagazineCount = magazineCapacity / 2;

		int status = object_depot_init(&depot, magazineCapacity,
				maxMagazineCount, flags_, this,
				object_cache_return_object_wrapper);

		if (status != 0) {
			return status;
		}
	}

	this->cookie = cookie_;
	this->constructor = constructor_;
	this->destructor = destructor_;
	this->reclaimer = reclaimer_;

	return 0;
}


slab* ObjectCache::InitSlab(slab* slab, void* pages, size_t byteCount, [[maybe_unused]] uint32_t flags_)
{
	slab->pages = pages;
	slab->count = slab->size = byteCount / object_size;
	slab->free = nullptr;

	size_t spareBytes = byteCount - (slab->size * object_size);

	slab->offset = cache_color_cycle;

	cache_color_cycle += alignment;
	if (cache_color_cycle > spareBytes)
		cache_color_cycle = 0;

	uint8_t * data = ((uint8_t *)pages) + slab->offset;

	for (size_t i = 0; i < slab->size; i++) {
		int status = 0;
		if (constructor)
			status = constructor(cookie, data);

		if (status != 0) {
			data = ((uint8_t *)pages) + slab->offset;
			for (size_t j = 0; j < i; j++) {
				if (destructor)
					destructor(cookie, data);
				data += object_size;
			}

			return nullptr;
		}

		fill_allocated_block(data, object_size);
		MemoryManager::_push(slab->free, object_to_link(data, object_size));

		data += object_size;
	}

	return slab;
}

void
ObjectCache::UninitSlab(slab* slab)
{
	if (slab->count != slab->size)
		panic("cache: destroying a slab which isn't empty.");

	usage -= slab_size;
	total_objects -= slab->size;

	uint8_t * data = ((uint8_t *)slab->pages) + slab->offset;

	for (size_t i = 0; i < slab->size; i++) {
		if (destructor)
			destructor(cookie, data);
		data += object_size;
	}
}


void ObjectCache::ReturnObjectToSlab(slab* source, void* object, uint32_t flags_)
{
	if (source == nullptr) {
		panic("object_cache: free'd object %p has no slab", object);
	}

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	uint8_t* objectsStart = (uint8_t*)source->pages + source->offset;
	if (object < objectsStart
		|| object >= objectsStart + source->size * object_size
		|| ((uint8_t*)object - objectsStart) % object_size != 0) {
		panic("object_cache: tried to free invalid object pointer %p", object);
	}
#endif // KDEBUG

	object_link*elementLink = object_to_link(object, object_size);

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	size_t objectCount = 0;
	for (auto *olink = source->free; olink; olink = olink->next)
	{
		if (olink == elementLink)
		{
			panic("double free\n");
		}

		objectCount ++;
	}

	if (objectCount != source->count)
		panic("slab damaged\n");
#endif

	// no-op in non-debug builds, otherwise fills the memory with a pattern that is likely
	// to crash anything using unallocated memory
	fill_allocated_block(object, object_size);

	MemoryManager::_push(source->free, elementLink);
	source->count++;
	used_count--;

	if (source->count == source->size) {
		partial.Remove(source);

		if(flags_ & CACHE_DONT_LOCK_KERNEL)
		{
			// Free called in non-preemptible context so it's not safe to release memory here
			empty_count++;
			empty.Add(source);
		} else if (empty_count < pressure
			&& total_objects - used_count - source->size
				>= min_object_reserve)
		{
			empty_count++;
			empty.Add(source);
		} else {
			ReturnSlab(source, flags_);
		}
	} else if (source->count == 1) {
		full.Remove(source);
		partial.Add(source);
	}
}

void*
ObjectCache::ObjectAtIndex(slab* source, int32_t index) const
{
	return (uint8_t *)source->pages + source->offset + index * object_size;
}

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS

bool
ObjectCache::AssertObjectNotFreed(void* object)
{
	MutexLocker locker(lock);

	slab* source = ObjectSlab(object);
	if (!partial.Contains(source) && !full.Contains(source)) {
		panic("object_cache: to be freed object %p: slab not part of cache!",
			object);
	}

	object_link* checkedLink = object_to_link(object, object_size);
	for (object_link* freeLink = source->free; freeLink != nullptr;
			freeLink = freeLink->next) {
		if (freeLink == checkedLink) {
			panic("object_cache: double free of %p (slab %p, cache %p)",
				object, source, this);
		}
	}

	return true;
}

#endif // CONFIG_DEBUG_MEMALLOCATOR_CHECKS

/*static*/ SmallObjectCache*
SmallObjectCache::Create(const char* name, size_t object_size,
	size_t alignment, size_t maximum, size_t magazineCapacity,
	size_t maxMagazineCount, uint32_t flags, void* cookie,
	object_cache_constructor constructor, object_cache_destructor destructor,
	object_cache_reclaimer reclaimer)
{
	void* buffer = slab_internal_alloc(sizeof(SmallObjectCache), flags);
	if (buffer == nullptr)
		return nullptr;

	SmallObjectCache* cache = new(buffer) SmallObjectCache();

	cache->Init(name, object_size, alignment, maximum, magazineCapacity,
			maxMagazineCount, flags, cookie, constructor, destructor,
			reclaimer);

	if ((flags & CACHE_LARGE_SLAB) != 0)
		cache->slab_size = 1024 * object_size;
	else
		cache->slab_size = SLAB_CHUNK_SIZE_SMALL;

	cache->slab_size = MemoryManager::AcceptableChunkSize(cache->slab_size);

	return cache;
}

void SmallObjectCache::Delete()
{
	this->~SmallObjectCache();
	slab_internal_free(this, 0);
}

slab* SmallObjectCache::CreateSlab(uint32_t createFlags)
{
	if (!check_cache_quota(this))
		return nullptr;

	void* pages;

	Unlock();
	int error;

	error = MemoryManager::Allocate(this, createFlags, pages);

	Lock();

	if (error != 0)
		return nullptr;

	slab* newSlab = slab_in_pages(pages, slab_size);
	size_t byteCount = slab_size - sizeof(slab);

	return InitSlab(newSlab, pages, byteCount, createFlags);
}

void SmallObjectCache::ReturnSlab(slab* slab, uint32_t freeFLags)
{
	UninitSlab(slab);

	Unlock();

	MemoryManager::Free(slab->pages, freeFLags);

	Lock();
}

slab* SmallObjectCache::ObjectSlab(void* object) const
{
	return slab_in_pages(lower_boundary(object, slab_size), slab_size);
}

static inline int
__fls0(size_t value)
{
	if (value == 0)
		return -1;

	int bit;
	for (bit = 0; value != 1; bit++)
		value >>= 1;
	return bit;
}


static HashedSlab*
allocate_slab(uint32_t flags)
{
	return (HashedSlab*)slab_internal_alloc(sizeof(HashedSlab), flags);
}


static void
free_slab(HashedSlab* slab, uint32_t flags)
{
	slab_internal_free(slab, flags);
}


HashedObjectCache::HashedObjectCache()
	:
	hash_table(this)
{
}


/*static*/ HashedObjectCache*
HashedObjectCache::Create(const char* name, size_t object_size,
	size_t alignment, size_t maximum, size_t magazineCapacity,
	size_t maxMagazineCount, uint32_t flags, void* cookie,
	object_cache_constructor constructor, object_cache_destructor destructor,
	object_cache_reclaimer reclaimer)
{
	void* buffer = slab_internal_alloc(sizeof(HashedObjectCache), flags);
	if (buffer == nullptr)
		return nullptr;

	HashedObjectCache* cache = new(buffer) HashedObjectCache();

	// init the hash table
	size_t hashSize = cache->hash_table.ResizeNeeded();
	buffer = slab_internal_alloc(hashSize, flags);
	if (buffer == nullptr) {
		cache->Delete();
		return nullptr;
	}

	cache->hash_table.Resize(buffer, hashSize, true);

	cache->Init(name, object_size, alignment, maximum, magazineCapacity,
			maxMagazineCount, flags, cookie, constructor, destructor,
			reclaimer);

	if ((flags & CACHE_LARGE_SLAB) != 0)
		cache->slab_size = 128 * object_size;
	else
		cache->slab_size = 8 * object_size;

	cache->slab_size = MemoryManager::AcceptableChunkSize(cache->slab_size);
	cache->lower_boundary = __fls0(cache->slab_size);

	return cache;
}


void
HashedObjectCache::Delete()
{
	this->~HashedObjectCache();
	slab_internal_free(this, 0);
}


slab*
HashedObjectCache::CreateSlab(uint32_t createFlags)
{
	if (!check_cache_quota(this))
		return nullptr;

	Unlock();

	HashedSlab* slab = allocate_slab(createFlags);
	if (slab != nullptr) {
		void* pages = nullptr;
		if (MemoryManager::Allocate(this, createFlags, pages) == 0) {
			Lock();
			if (InitSlab(slab, pages, slab_size, createFlags)) {
				hash_table.InsertUnchecked(slab);
				_ResizeHashTableIfNeeded(createFlags);
				return slab;
			}
			Unlock();
		}

		if (pages != nullptr)
			MemoryManager::Free(pages, createFlags);

		free_slab(slab, createFlags);
	}

	Lock();
	return nullptr;
}


void
HashedObjectCache::ReturnSlab(slab* _slab, uint32_t freeFlags)
{
	HashedSlab* slab = static_cast<HashedSlab*>(_slab);

	hash_table.RemoveUnchecked(slab);
	_ResizeHashTableIfNeeded(freeFlags);

	UninitSlab(slab);

	Unlock();
	MemoryManager::Free(slab->pages, freeFlags);
	free_slab(slab, freeFlags);
	Lock();
}


slab*
HashedObjectCache::ObjectSlab(void* object) const
{
	HashedSlab* slab = hash_table.Lookup(::lower_boundary(object, slab_size));
	if (slab == nullptr) {
		panic("hash object cache %p: unknown object %p", this, object);
	}

	return slab;
}


void
HashedObjectCache::_ResizeHashTableIfNeeded(uint32_t resizeFlags)
{
	size_t hashSize = hash_table.ResizeNeeded();
	if (hashSize != 0) {
		Unlock();
		void* buffer = slab_internal_alloc(hashSize, resizeFlags);
		Lock();

		if (buffer != nullptr) {
			if (hash_table.ResizeNeeded() == hashSize) {
				void* oldHash;
				hash_table.Resize(buffer, hashSize, true, &oldHash);
				if (oldHash != nullptr) {
					Unlock();
					slab_internal_free(oldHash, resizeFlags);
					Lock();
				}
			}
		}
	}
}

static int
size_to_index(size_t size)
{
	if (size <= 16)
		return 0;
	if (size <= 32)
		return 1 + (size - 16 - 1) / 8;
	if (size <= 128)
		return 3 + (size - 32 - 1) / 16;
	if (size <= 256)
		return 9 + (size - 128 - 1) / 32;
	if (size <= 512)
		return 13 + (size - 256 - 1) / 64;
	if (size <= 1024)
		return 17 + (size - 512 - 1) / 128;
	if (size <= 2048)
		return 21 + (size - 1024 - 1) / 256;
	if (size <= 8192)
		return 25 + (size - 2048 - 1) / 512;

	return -1;
}

void* block_alloc(size_t size, size_t alignment, uint32_t flags)
{
	if (!arch_kernel_allocation_size_valid(size)) {
		// Cap invalid size
		return nullptr;
	}

	if (alignment > alignof(void*)) {
		// Make size >= alignment and a power of two. This is sufficient, since
		// all of our object caches with power of two sizes are aligned. We may
		// waste quite a bit of memory, but memalign() is very rarely used
		// in the kernel and always with power of two size == alignment anyway.
		ASSERT((alignment & (alignment - 1)) == 0);
		while (alignment < size)
			alignment <<= 1;
		size = alignment;

		// If we're not using an object cache, make sure that the memory
		// manager knows it has to align the allocation.
		if (size > kBlockSizes[kNumBlockSizes])
			flags |= CACHE_ALIGN_ON_SIZE;
	}

	// allocate from the respective object cache, if any
	if (size > PAGE_SIZE
			&& (flags & CACHE_CONTIGUOUS_MEMORY) == CACHE_CONTIGUOUS_MEMORY) {
	} else {
		int index = size_to_index(size);

		if (index >= 0)
			return object_cache_alloc(sBlockCaches[index], flags & ~CACHE_CONTIGUOUS_MEMORY);
	}

	if (flags & CACHE_DONT_LOCK_KERNEL) {
		printk(KERN_ERR, "Large allocations not allowed in locked context\n");
		return nullptr;
	}

	// the allocation is too large for our object caches -- ask the memory
	// manager
	void* block;
	if (MemoryManager::AllocateRaw(size, flags | CACHE_DONT_OOM_KILL, block) != 0)
		return nullptr;

	return block;
}

void* block_alloc_early(size_t size)
{
	if (!arch_kernel_allocation_size_valid(size)) {
		// Cap invalid size
		return nullptr;
	}

	int index = size_to_index(size);
	if (index >= 0 && sBlockCaches[index] != nullptr)
		return object_cache_alloc(sBlockCaches[index], CACHE_DURING_BOOT);

	if (size > SLAB_CHUNK_SIZE_SMALL) {
		// This is a sufficiently large allocation -- just ask the memory
		// manager directly.
		void* block;
		if (MemoryManager::AllocateRaw(size, CACHE_DONT_OOM_KILL, block) != 0)
			return nullptr;

		return block;
	}

	// A small allocation, but no object cache yet. Use the bootstrap memory.
	// This allocation must never be freed!
	if (sBootStrapMemorySize - sUsedBootStrapMemory < size) {
		// We need more memory.
		void* block;
		if (MemoryManager::AllocateRaw(SLAB_CHUNK_SIZE_SMALL, CACHE_DONT_OOM_KILL, block) != 0)
			return nullptr;
		sBootStrapMemory = (uintptr_t)block;
		sBootStrapMemorySize = SLAB_CHUNK_SIZE_SMALL;
		sUsedBootStrapMemory = 0;
	}

	size_t neededSize = roundup(size, sizeof(double));
	if (sUsedBootStrapMemory + neededSize > sBootStrapMemorySize)
		return nullptr;
	void* block = (void*)(sBootStrapMemory + sUsedBootStrapMemory);
	sUsedBootStrapMemory += neededSize;

	return block;
}

void block_free(void* block, uint32_t flags)
{
	if (block == nullptr)
		return;

	if (!are_interrupts_enabled() && !gKernelStartup) {
		free_deferred(block);
		return;
	}

	ObjectCache *cache = MemoryManager::FreeRawOrReturnCache(block, flags);
	if (cache != nullptr) {
		// a regular small allocation
		ASSERT(cache->object_size >= kBlockSizes[0]);
		ASSERT(cache->object_size <= kBlockSizes[kNumBlockSizes - 1]);
		ASSERT(cache == sBlockCaches[size_to_index(cache->object_size)]);
		object_cache_free(cache, block, flags);
	}
}

void block_allocator_init_boot(void)
{
	for (int index = 0; kBlockSizes[index] != 0; index++) {
		char name[32];
		snprintf(name, sizeof(name), "block allocator: %zu",
			kBlockSizes[index]);

		uint32_t flags = CACHE_DURING_BOOT;
		size_t size = kBlockSizes[index];

		// align the power of two objects to their size
		size_t alignment = (size & (size - 1)) == 0 ? size : 0;

		// For the larger allocation sizes disable the object depot, so we don't
		// keep lot's of unused objects around.
		if (size > 2048)
			flags |= CACHE_NO_DEPOT;

		sBlockCaches[index] = create_object_cache_etc(name, size, alignment, 0,
			0, 0, flags, nullptr, nullptr, nullptr, nullptr);

		if (sBlockCaches[index] == nullptr)
			panic("allocator: failed to init block cache");
	}
}

static void
delete_object_cache_internal(ObjectCache * cache)
{
	if (!(cache->flags & CACHE_NO_DEPOT))
		object_depot_destroy(&cache->depot, 0);

	mutex_lock(&cache->lock);

	if (!cache->full.IsEmpty())
		panic("cache destroy: still has full slabs");

	if (!cache->partial.IsEmpty())
		panic("cache destroy: still has partial slabs");

	while (!cache->empty.IsEmpty())
		cache->ReturnSlab(cache->empty.RemoveHead(), 0);

	mutex_unlock(&cache->lock);

	cache->Delete();
}

/*!	Makes sure that \a objectCount objects can be allocated.
*/
static int object_cache_reserve_internal(ObjectCache* cache, size_t objectCount, uint32_t flags)
{
	// If someone else is already adding slabs, we wait for that to be finished
	// first.
	while (true) {
		if (objectCount <= cache->total_objects - cache->used_count)
			return 0;

		ObjectCacheResizeEntry* resizeEntry = nullptr;
		if (cache->resize_entry_dont_wait != nullptr) {
			resizeEntry = cache->resize_entry_dont_wait;
			if (thread_get_current_thread_id() == resizeEntry->thread)
				return -EWOULDBLOCK;
			// Note: We could still have reentered the function, i.e.
			// resize_entry_can_wait would be ours. That doesn't matter much,
			// though, since after the don't-wait thread has done its job
			// everyone will be happy.
		} else if (cache->resize_entry_can_wait != nullptr) {
			resizeEntry = cache->resize_entry_can_wait;
			if (thread_get_current_thread_id() == resizeEntry->thread)
				return -EWOULDBLOCK;

			if ((flags & CACHE_DONT_WAIT_FOR_MEMORY) != 0)
				break;
		} else
			break;

		DEFINE_CONDVAR_ENTRY(entry);
		resizeEntry->condition.Add(&entry);

		cache->Unlock();
		entry.Wait();
		cache->Lock();
	}

	// prepare the resize entry others can wait on
	ObjectCacheResizeEntry*& resizeEntry
		= (flags & CACHE_DONT_WAIT_FOR_MEMORY) != 0
			? cache->resize_entry_dont_wait : cache->resize_entry_can_wait;

	ObjectCacheResizeEntry myResizeEntry;
	resizeEntry = &myResizeEntry;
	resizeEntry->condition.Init(cache, "wait for slabs");
	resizeEntry->thread = thread_get_current_thread_id();

	// add new slabs until there are as many free ones as requested
	while (objectCount > cache->total_objects - cache->used_count) {
		slab* newSlab = cache->CreateSlab(flags);
		if (newSlab == nullptr) {
			resizeEntry->condition.NotifyAll();
			resizeEntry = nullptr;
			return -ENOMEM;
		}

		cache->usage += cache->slab_size;
		cache->total_objects += newSlab->size;

		cache->empty.Add(newSlab);
		cache->empty_count++;
	}

	resizeEntry->condition.NotifyAll();
	resizeEntry = nullptr;

	return 0;
}

static void object_cache_low_memory(void*, uint32_t, int32_t level)
{
	if (level == B_NO_LOW_RESOURCE)
		return;

	MutexLocker cacheListLocker(sObjectCacheListLock);

	// Append the first cache to the end of the queue. We assume that it is
	// one of the caches that will never be deleted and thus we use it as a
	// marker.
	ObjectCache* firstCache = STAILQ_FIRST(&sObjectCaches);
	STAILQ_REMOVE_HEAD(&sObjectCaches, link);
	STAILQ_INSERT_TAIL(&sObjectCaches, firstCache, link);
	cacheListLocker.Unlock();

	ObjectCache* cache;
	do {
		cacheListLocker.Lock();

		cache = STAILQ_FIRST(&sObjectCaches);
		STAILQ_REMOVE_HEAD(&sObjectCaches, link);
		STAILQ_INSERT_TAIL(&sObjectCaches, cache, link);

		MutexLocker maintenanceLocker(sMaintenanceLock);

		if (cache->maintenance_pending || cache->maintenance_in_progress) {
			// We don't want to mess with caches in maintenance.
			continue;
		}

		cache->maintenance_pending = true;
		cache->maintenance_in_progress = true;

		maintenanceLocker.Unlock();
		cacheListLocker.Unlock();

		// We are calling the reclaimer without the object cache lock
		// to give the owner a chance to return objects to the slabs.

		if (cache->reclaimer)
			cache->reclaimer(cache->cookie, level);

		if ((cache->flags & CACHE_NO_DEPOT) == 0)
			object_depot_make_empty(&cache->depot, 0);

		MutexLocker cacheLocker(cache->lock);
		size_t minimumAllowed;

		switch (level) {
			case B_LOW_RESOURCE_NOTE:
				minimumAllowed = cache->pressure / 2 + 1;
				cache->pressure -= cache->pressure / 8;
				break;

			case B_LOW_RESOURCE_WARNING:
				cache->pressure /= 2;
				minimumAllowed = 0;
				break;

			default:
				cache->pressure = 0;
				minimumAllowed = 0;
				break;
		}

		while (cache->empty_count > minimumAllowed) {
			// make sure we respect the cache's minimum object reserve
			size_t objectsPerSlab = cache->empty.Head()->size;
			size_t freeObjects = cache->total_objects - cache->used_count;
			if (freeObjects < cache->min_object_reserve + objectsPerSlab)
				break;

			cache->ReturnSlab(cache->empty.RemoveHead(), 0);
			cache->empty_count--;
		}

		cacheLocker.Unlock();

		// Check whether in the meantime someone has really requested
		// maintenance for the cache.
		maintenanceLocker.Lock();

		if (cache->maintenance_delete) {
			delete_object_cache_internal(cache);
			continue;
		}

		cache->maintenance_in_progress = false;

		if (cache->maintenance_resize)
			STAILQ_INSERT_TAIL(&sMaintenanceQueue, cache, link);
		else
			cache->maintenance_pending = false;
	} while (cache != firstCache);
}

void mm_perform_maintenance_thread_work(void)
{
	MutexLocker locker(sMaintenanceLock);

	// wait for the next request
	while (STAILQ_EMPTY(&sMaintenanceQueue)) {
		// perform memory manager maintenance, if needed
		if (MemoryManager::sMaintenanceNeeded) {
			locker.Unlock();
			MemoryManager::PerformMaintenance();
			locker.Lock();
			continue;
		}

		if(!scheduler_scheduling_enabled()) {
			if(STAILQ_EMPTY(&sMaintenanceQueue))
				return;
			break;
		}

		DEFINE_CONDVAR_ENTRY(entry);
		MemoryManager::sMaintenanceCond.Add(&entry);
		locker.Unlock();
		entry.Wait();
		locker.Lock();
	}

	ObjectCache* cache = STAILQ_FIRST(&sMaintenanceQueue);
	STAILQ_REMOVE_HEAD(&sMaintenanceQueue, maintenance_link);

	while (true) {
		bool resizeRequested = cache->maintenance_resize;
		bool deleteRequested = cache->maintenance_delete;

		if (!resizeRequested && !deleteRequested) {
			cache->maintenance_pending = false;
			cache->maintenance_in_progress = false;
			break;
		}

		cache->maintenance_resize = false;
		cache->maintenance_in_progress = true;

		locker.Unlock();

		if (deleteRequested) {
			delete_object_cache_internal(cache);
			break;
		}

		// resize the cache, if necessary

		AutoLocker<ObjectCache> cacheLocker(cache);

		if (resizeRequested) {
			/* Kill tasks only if we can't reserve for small objects */
			int error = object_cache_reserve_internal(cache,
				cache->min_object_reserve, 
				cache->object_size <= PAGE_SIZE ? 0 : CACHE_DONT_OOM_KILL);
			if (error != 0) {
				printk(KERN_ERR, "object cache resizer: Failed to resize object "
					"cache %p!\n", cache);
				break;
			}
		}

		cacheLocker.Unlock();
		locker.Lock();
	}
}

[[noreturn]] static int object_cache_maintainer(void*)
{
	while (true) {
		mm_perform_maintenance_thread_work();
	}
}


ObjectCache * create_object_cache(const char* name, size_t object_size, size_t alignment,
	void* cookie, object_cache_constructor constructor,
	object_cache_destructor destructor)
{
	return create_object_cache_etc(name, object_size, alignment, 0, 0, 0, 0,
		cookie, constructor, destructor, nullptr);
}


ObjectCache* create_object_cache_etc(const char* name, size_t objectSize, size_t alignment,
	size_t maximum, size_t magazineCapacity, size_t maxMagazineCount,
	uint32_t flags, void* cookie, object_cache_constructor constructor,
	object_cache_destructor destructor, object_cache_reclaimer reclaimer)
{
	ObjectCache* cache;

	if (objectSize == 0) {
		cache = nullptr;
	} else if (objectSize <= 256) {
		cache = SmallObjectCache::Create(name, objectSize, alignment, maximum,
			magazineCapacity, maxMagazineCount, flags, cookie, constructor,
			destructor, reclaimer);
	} else {
		cache = HashedObjectCache::Create(name, objectSize, alignment, maximum,
			magazineCapacity, maxMagazineCount, flags, cookie, constructor,
			destructor, reclaimer);
	}

	if (cache != nullptr) {
		MutexLocker _(sObjectCacheListLock);
		STAILQ_INSERT_TAIL(&sObjectCaches, cache, link);
	}

	return cache;
}

void delete_object_cache(ObjectCache * cache)
{
	{
		MutexLocker _(sObjectCacheListLock);
		STAILQ_REMOVE(&sObjectCaches, cache, ObjectCache, link);
	}

	{
		MutexLocker cacheLocker(cache->lock);
		MutexLocker maintenanceLocker(sMaintenanceLock);

		if (cache->maintenance_in_progress) {
			// The maintainer thread is working with the cache. Just mark it
			// to be deleted.
			cache->maintenance_delete = true;
			return;
		}

		// unschedule maintenance
		if (cache->maintenance_pending)
			STAILQ_REMOVE(&sMaintenanceQueue, cache, ObjectCache, maintenance_link);
	}

	// at this point no-one should have a reference to the cache anymore

	delete_object_cache_internal(cache);
}

int  object_cache_set_minimum_reserve(struct ObjectCache * cache, size_t objectCount)
{
	MutexLocker _(cache->lock);

	if (cache->min_object_reserve == objectCount)
		return 0;

	cache->min_object_reserve = objectCount;

	increase_object_reserve(cache);

	return 0;
}


void* object_cache_alloc(struct ObjectCache * cache, uint32_t flags)
{
	if (!are_interrupts_enabled() && !gKernelStartup) {
		panic("object_cache_alloc: Called from non-preemptible context\n");
	}

	if (!(cache->flags & CACHE_NO_DEPOT)) {
		void* object = object_depot_obtain(&cache->depot);
		if (object) {
			if(!(flags & CACHE_CLEAR_MEMORY))
				return fill_allocated_block(object, cache->object_size);
			memset(object, 0, cache->object_size);
			return object;
		}
	}

	MutexLocker locker(cache->lock);

	slab* source = nullptr;

	while (true) {
		source = cache->partial.Head();
		if (source != nullptr)
			break;

		source = cache->empty.RemoveHead();
		if (source != nullptr) {
			cache->empty_count--;
			cache->partial.Add(source);
			break;
		}

		if (object_cache_reserve_internal(cache, 1, flags) != 0) {
			return nullptr;
		}

		cache->pressure++;
	}

	if (nullptr == source->free)
		panic("slab damaged\n");

	object_link* link = MemoryManager::_pop(source->free);
	source->count--;
	cache->used_count++;

	if (cache->total_objects - cache->used_count < cache->min_object_reserve)
		increase_object_reserve(cache);

	if (source->count == 0) {
		cache->partial.Remove(source);
		cache->full.Add(source);
	}

	void* object = link_to_object(link, cache->object_size);
	locker.Unlock();

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	uint32_t *objectData = (uint32_t *)object;
	for (size_t i = 0; i < (cache->object_size - sizeof(object_link)) / 4; i++)
		if (objectData[i] != _fill_block_pattern)
			panic("slab memory guard error\n");
#endif

	if(!(flags & CACHE_CLEAR_MEMORY))
		return fill_allocated_block(object, cache->object_size);

	memset(object, 0, cache->object_size);
	return object;
}

void object_cache_free(struct ObjectCache* cache, void* object, uint32_t flags)
{
	if (object == nullptr)
		return;

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	// TODO: allow forcing the check even if we don't find deadbeef
	if (*(uint32_t *)object == 0xdeadbeef) {
		if (!cache->AssertObjectNotFreed(object))
			return;

		if ((cache->flags & CACHE_NO_DEPOT) == 0) {
			if (object_depot_contains_object(&cache->depot, object)) {
				panic("object_cache: object %p is already freed", object);
			}
		}
	}

	fill_freed_block(object, cache->object_size);
#endif

	if ((cache->flags & CACHE_NO_DEPOT) == 0) {
		object_depot_store(&cache->depot, object, flags);
		return;
	}

	MutexLocker _(cache->lock);

	cache->ReturnObjectToSlab(cache->ObjectSlab(object), object, flags);
}

int object_cache_reserve(struct ObjectCache* cache, size_t objectCount, uint32_t flags)
{
	if (objectCount == 0)
		return 0;

	MutexLocker _(cache->lock);

	return object_cache_reserve_internal(cache, objectCount, flags);
}


#if defined(CONFIG_KDEBUGGER)
static void
dump_slab(::slab* slab)
{
	kprintf("  %p  %p  %6zu %6zu %6zu  %p\n",
		slab, slab->pages, slab->size, slab->count, slab->offset, slab->free);
}


static int
dump_slabs(int, char*[])
{
	kprintf("%*s %22s %8s %8s %8s %6s %8s %8s %8s\n",
		B_PRINTF_POINTER_WIDTH + 2, "address", "name", "objsize", "align",
		"usage", "empty", "usedobj", "total", "flags");

	ObjectCache * cache;
	STAILQ_FOREACH(cache, &sObjectCaches, link) {
		kprintf("%p %22s %8zu %8zu %8zu %6zu %8zu %8zu %8" PRIx32
			"\n", cache, cache->name, cache->object_size, cache->alignment,
			cache->usage, cache->empty_count, cache->used_count,
			cache->total_objects, cache->flags);
	}

	return 0;
}

void dump_object_depot(object_depot* depot);
int dump_object_depot(int argCount, char** args);
int dump_depot_magazine(int argCount, char** args);

static int
dump_cache_info(int argc, char* argv[])
{
	if (argc < 2) {
		kprintf("usage: slab_cache [address]\n");
		return 0;
	}

	ObjectCache* cache = (ObjectCache*)parse_expression(argv[1]);

	kprintf("name:              %s\n", cache->name);
	kprintf("lock:              %p\n", &cache->lock);
	kprintf("object_size:       %zu\n", cache->object_size);
	kprintf("alignment:         %zu\n", cache->alignment);
	kprintf("cache_color_cycle: %zu\n", cache->cache_color_cycle);
	kprintf("total_objects:     %zu\n", cache->total_objects);
	kprintf("used_count:        %zu\n", cache->used_count);
	kprintf("empty_count:       %zu\n", cache->empty_count);
	kprintf("pressure:          %zu\n", cache->pressure);
	kprintf("slab_size:         %zu\n", cache->slab_size);
	kprintf("usage:             %zu\n", cache->usage);
	kprintf("maximum:           %zu\n", cache->maximum);
	kprintf("flags:             0x%" PRIx32 "\n", cache->flags);
	kprintf("cookie:            %p\n", cache->cookie);
	kprintf("resize entry don't wait: %p\n", cache->resize_entry_dont_wait);
	kprintf("resize entry can wait:   %p\n", cache->resize_entry_can_wait);

	kprintf("  %-*s    %-*s      size   used offset  free\n",
		B_PRINTF_POINTER_WIDTH, "slab", B_PRINTF_POINTER_WIDTH, "chunk");

	SlabList::Iterator iterator = cache->empty.GetIterator();
	if (iterator.HasNext())
		kprintf("empty:\n");
	while (::slab* slab = iterator.Next())
		dump_slab(slab);

	iterator = cache->partial.GetIterator();
	if (iterator.HasNext())
		kprintf("partial:\n");
	while (::slab* slab = iterator.Next())
		dump_slab(slab);

	iterator = cache->full.GetIterator();
	if (iterator.HasNext())
		kprintf("full:\n");
	while (::slab* slab = iterator.Next())
		dump_slab(slab);

	if ((cache->flags & CACHE_NO_DEPOT) == 0) {
		kprintf("depot:\n");
		dump_object_depot(&cache->depot);
	}

	return 0;
}
#endif

void __init_text object_cache_init_post_thread(void) {
	if(register_low_resource_handler(object_cache_low_memory, nullptr,
			B_KERNEL_RESOURCE_PAGES | B_KERNEL_RESOURCE_MEMORY
					| B_KERNEL_RESOURCE_ADDRESS_SPACE, 5) < 0)
	{
		panic("Can't register object cache lowres");
	}

	tid_t id = spawn_kernel_thread_etc(object_cache_maintainer,
			"object cache resizer", K_URGENT_PRIORITY, nullptr, 0);

	if(id < 0)
		panic("Can't spawn object cache maintainer");

	resume_thread(id);

#if defined(CONFIG_KDEBUGGER)
	add_debugger_command("slabs", dump_slabs, "list all object caches");
	add_debugger_command("slab_cache", dump_cache_info,
			"dump information about a specific object cache");
	add_debugger_command("slab_depot", dump_object_depot,
			"dump contents of an object depot");
	add_debugger_command("slab_magazine", dump_depot_magazine,
			"dump contents of a depot magazine");
#endif
}

void* malloc_etc(size_t size, uint32_t flags)
{
	return block_alloc(size, 0, flags);
}

void* memalign(size_t alignment, size_t size)
{
	return block_alloc(size, alignment, 0);
}


void * memalign_etc(size_t alignment, size_t size, uint32_t flags)
{
	return block_alloc(size, alignment, flags);
}


extern "C" int __posix_memalign(void** _pointer, size_t alignment, size_t size)
{
	if(_pointer == nullptr)
		return EINVAL;
	if(!powerof2(alignment))
		return EINVAL;
	void * mem = block_alloc(size, alignment, 0);
	if(mem == nullptr)
		return ENOMEM;
	*_pointer = mem;
	return 0;
}

__weak_reference(__posix_memalign, posix_memalign);

void free_etc(void *address, uint32_t flags)
{
	block_free(address, flags);
}


void free(void *addr) {
#if defined(CONFIG_KDEBUGGER)
	/*
	 * In case GDB is attached during kernel crash
	 * it might want to allocate some memory
	 */
	if(debug_debugger_running()) {
		debug_free(addr);
		return;
	}
#endif

	block_free(addr, 0);
}

void *malloc(size_t size) {
#if defined(CONFIG_KDEBUGGER)
	/*
	 * In case GDB is attached during kernel crash
	 * it might want to allocate some memory
	 */
	if(debug_debugger_running()) {
		return debug_malloc(size);
	}
#endif

	return block_alloc(size, 0, 0);
}

void *realloc_etc(void *address, size_t newSize, int flags) {
	if (!arch_kernel_allocation_size_valid(newSize)) {
		// Avoid internal roundings
		return nullptr;
	}

	if (newSize == 0) {
		block_free(address, flags);
		return nullptr;
	}

	if (address == nullptr)
		return block_alloc(newSize, 0, flags);

	size_t oldSize;
	ObjectCache* cache = MemoryManager::GetAllocationInfo(address, oldSize);
	if (cache == nullptr && oldSize == 0) {
		panic("block_realloc(): allocation %p not known", address);
	}

	if (oldSize == newSize)
		return address;

	void* newBlock = block_alloc(newSize, 0, flags);
	if (newBlock == nullptr)
		return nullptr;

	memcpy(newBlock, address, MIN(oldSize, newSize));

	block_free(address, flags);

	return newBlock;
}

void *realloc(void *addr, size_t size) {
	return realloc_etc(addr, size, 0);
}

void * vmalloc_etc(size_t length, int, int wait_flags) {
	return block_alloc(length, PAGE_SIZE, wait_flags);
}

__warn_references(vmalloc_etc, "malloc_etc is preferred");

void * vmalloc(size_t length) {
	return block_alloc(length, PAGE_SIZE, 0);
}

__warn_references(vmalloc, "This API is for compatibility only. Use malloc() instead");

void * vzalloc(size_t length) {
	return block_alloc(length, PAGE_SIZE, CACHE_CLEAR_MEMORY);
}

__warn_references(vzalloc, "This API is for compatibility only. Use calloc() instead");

void vfree(void * pointer) {
	block_free(pointer, 0);
}

__warn_references(vfree, "This API is for compatibility only. Use free() instead");

void *calloc(size_t n_elements, size_t elem_size) {
	size_t req = 0;

#if defined(CONFIG_KDEBUGGER)
	/*
	 * In case GDB is attached during kernel crash
	 * it might want to allocate some memory
	 */
	if(debug_debugger_running()) {
		return debug_calloc(n_elements, elem_size);
	}
#endif

	if (n_elements != 0) {
		req = n_elements * elem_size;
		if (((n_elements | elem_size) & ~(size_t) 0xffff)
				&& (req / n_elements != elem_size)) {
			req = SIZE_MAX; /* force downstream failure on overflow */
		}
	}

	return block_alloc(req, 0, CACHE_CLEAR_MEMORY);
}

void object_cache_print_debug([[maybe_unused]] backtrace_output& out) {
#if defined(CONFIG_DEBUG)
	out.print("%*s %22s %8s %8s %8s %6s %8s %8s %8s\n",
		B_PRINTF_POINTER_WIDTH + 2, "address", "name", "objsize", "align",
		"usage", "empty", "usedobj", "total", "flags");

	MutexLocker locker(sObjectCacheListLock);

	ObjectCache * cache;
	STAILQ_FOREACH(cache, &sObjectCaches, link) {
		out.print("%p %22s %8zu %8zu %8zu %6zu %8zu %8zu %8" PRIx32
			"\n", cache, cache->name, cache->object_size, cache->alignment,
			cache->usage, cache->empty_count, cache->used_count,
			cache->total_objects, cache->flags);
	}
#endif
}
