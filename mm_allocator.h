#ifndef _CORE_MM_ALLOCATOR_H_
#define _CORE_MM_ALLOCATOR_H_

#include <mm/mm_manager.h>
#include <mm/mm_depot.h>

static const size_t kMinObjectAlignment = MAX(alignof(void *), 8);

struct ObjectCache;
struct object_depot;

void*		block_alloc(size_t size, size_t alignment, uint32_t flags)
	__malloc_like __result_use_check __alloc_size(1) __alloc_align(2);
void*		block_alloc_early(size_t size)
	__malloc_like __result_use_check __alloc_size(1);
void		block_free(void* block, uint32_t flags);

struct ResizeRequest;

struct object_link {
	struct object_link* next;
};

struct slab  : DoublyLinkedListLinkImpl<slab> {
	void*				pages;
	size_t				size;		// total number of objects
	size_t				count;		// free objects
	size_t				offset;
	struct object_link*	free;
};

typedef DoublyLinkedList<slab> SlabList;

struct ObjectCacheResizeEntry {
	cond_var_t			condition;
	tid_t				thread;
};

struct ObjectCache {
	char				name[32];
	mutex_t			lock{"object_cache"};
	size_t				object_size;
	size_t				alignment;
	size_t				cache_color_cycle;
	SlabList			empty;
	SlabList			partial;
	SlabList			full;
	size_t				total_objects;		// total number of objects
	size_t				used_count;			// used objects
	size_t				empty_count;		// empty slabs
	size_t				pressure;
	size_t				min_object_reserve;
	size_t				slab_size;
	size_t				usage;
	size_t				maximum;
	uint32_t			flags;
	ResizeRequest*		resize_request;
	ObjectCacheResizeEntry* resize_entry_can_wait;
	ObjectCacheResizeEntry* resize_entry_dont_wait;
	STAILQ_ENTRY(ObjectCache) link;
	STAILQ_ENTRY(ObjectCache) maintenance_link;
	bool				maintenance_pending;
	bool				maintenance_in_progress;
	bool				maintenance_resize;
	bool				maintenance_delete;
	void*				cookie;
	object_cache_constructor	constructor;
	object_cache_destructor 	destructor;
	object_cache_reclaimer 		reclaimer;
	object_depot		depot;

	virtual ~ObjectCache() = default;

	int Init(const char*name_, size_t objectSize, size_t alignment_, size_t maximum_, size_t magazineCapacity,
			size_t maxMagazineCount, uint32_t flags_, void*cookie_, object_cache_constructor constructor_,
			object_cache_destructor destructor_, object_cache_reclaimer reclaimer_);

	virtual void Delete() = 0;

	virtual slab* CreateSlab(uint32_t flags) = 0;
	virtual void ReturnSlab(slab* slab, uint32_t flags) = 0;
	virtual slab* ObjectSlab(void* object) const = 0;

	slab* InitSlab(slab* slab, void* pages, size_t byteCount, uint32_t flags_);

	void UninitSlab(slab* slab);

	void ReturnObjectToSlab(slab* source, void* object, uint32_t flags_);
	void* ObjectAtIndex(slab* source, int32_t index) const;

	bool Lock() {  return mutex_lock(&lock) == 0; }
	void Unlock() { mutex_unlock(&lock); }

	int AllocatePages(void** pages, uint32_t flags);
	void FreePages(void* pages);
	int EarlyAllocatePages(void** pages, uint32_t flags);
	void EarlyFreePages(void* pages);

#if CONFIG_DEBUG_MEMALLOCATOR_CHECKS
	bool AssertObjectNotFreed(void* object);
#endif
};


static inline void*
link_to_object(object_link* link, size_t objectSize) {
	return ((uint8_t*) link) - (objectSize - sizeof(object_link));
}

static inline object_link* object_to_link(void* object, size_t objectSize) {
	return (object_link*) (((uint8_t*) object) + (objectSize - sizeof(object_link)));
}

static inline void* lower_boundary(const void* object, size_t byteCount) {
	return (void*) ((uintptr_t) object & ~(byteCount - 1));
}

static inline bool check_cache_quota(ObjectCache* cache) {
	if (cache->maximum == 0)
		return true;

	return (cache->usage + cache->slab_size) <= cache->maximum;
}

static inline void*
slab_internal_alloc(size_t size, uint32_t flags)
{
	if (flags & CACHE_DURING_BOOT)
		return block_alloc_early(size);

	return block_alloc(size, 0, flags);
}


static inline void
slab_internal_free(void* buffer, uint32_t flags)
{
	block_free(buffer, flags);
}

struct SmallObjectCache final : ObjectCache {
	static	SmallObjectCache*	Create(const char* name, size_t object_size,
									size_t alignment, size_t maximum,
									size_t magazineCapacity,
									size_t maxMagazineCount,
									uint32_t flags, void* cookie,
									object_cache_constructor constructor,
									object_cache_destructor destructor,
									object_cache_reclaimer reclaimer);
		void				Delete() override;

		slab*				CreateSlab(uint32_t createFlags) override;
		void				ReturnSlab(slab* slab, uint32_t freeFLags) override;
	slab*				ObjectSlab(void* object) const override;
};

struct HashedSlab : slab {
	HashedSlab*	hash_next;
};


struct HashedObjectCache final : ObjectCache {
								HashedObjectCache();

	static	HashedObjectCache*	Create(const char* name, size_t object_size,
									size_t alignment, size_t maximum,
									size_t magazineCapacity,
									size_t maxMagazineCount,
									uint32_t flags, void* cookie,
									object_cache_constructor constructor,
									object_cache_destructor destructor,
									object_cache_reclaimer reclaimer);
		void				Delete() override;

		slab*				CreateSlab(uint32_t createFlags) override;
		void				ReturnSlab(slab* slab, uint32_t freeFlags) override;
	slab*				ObjectSlab(void* object) const override;

private:
			struct Definition {
				using ParentType = HashedObjectCache;
				using KeyType = const void *;
				using ValueType = HashedSlab;

				Definition(HashedObjectCache*parent_)
					:
					parent(parent_)
				{
				}

				Definition(const Definition& definition) = default;

				size_t HashKey(const void* key) const
				{
					return (uintptr_t)::lower_boundary(key, parent->slab_size)
						>> parent->lower_boundary;
				}

				size_t Hash(HashedSlab* value) const
				{
					return HashKey(value->pages);
				}

				static constexpr bool Compare(const void* key, const HashedSlab* value)
				{
					return value->pages == key;
				}

				static constexpr HashedSlab*& GetLink(HashedSlab* value)
				{
					return value->hash_next;
				}

				HashedObjectCache*	parent;
			};

			struct InternalAllocator {
				[[nodiscard]] static void* Allocate(size_t size)
				{
					return slab_internal_alloc(size, 0);
				}

				static void Free(void* memory)
				{
					slab_internal_free(memory, 0);
				}
			};

			using HashTable = BOpenHashTable<Definition, false, false, InternalAllocator>;

			friend struct Definition;

private:
			void				_ResizeHashTableIfNeeded(uint32_t resizeFlags);

private:
			HashTable hash_table;
			size_t lower_boundary;
};

#endif /* _CORE_MM_ALLOCATOR_H_ */
