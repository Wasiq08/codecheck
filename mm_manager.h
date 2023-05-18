

#ifndef _CORE_MM_MANAGER_H_
#define _CORE_MM_MANAGER_H_

#include <mm/page.h>
#include <core/mutex.h>
#include <core/condvar.h>
#include <sys/param.h>

#if defined(__cplusplus)
#include <lib/OpenHashTable.h>

#define SLAB_CHUNK_SIZE_SMALL		PAGE_SIZE
#define SLAB_CHUNK_SIZE_MEDIUM		(8 * PAGE_SIZE)
#define SLAB_CHUNK_SIZE_LARGE		(32 * PAGE_SIZE)
#if defined(__i386__) || defined(__x86_64__)
#define SLAB_AREA_SIZE				(1024 * PAGE_SIZE)
#elif defined(__aarch64__)
#define SLAB_AREA_SIZE				(512 * PAGE_SIZE)
#else
#define SLAB_AREA_SIZE				(256 * PAGE_SIZE)
#endif
#define SLAB_AREA_STRUCT_OFFSET		PAGE_SIZE

#define SLAB_META_CHUNKS_PER_AREA			(SLAB_AREA_SIZE / SLAB_CHUNK_SIZE_LARGE)
#define SLAB_SMALL_CHUNKS_PER_META_CHUNK	(SLAB_CHUNK_SIZE_LARGE / SLAB_CHUNK_SIZE_SMALL)

struct MemoryArea;
struct VMArea;

struct ObjectCache;

struct MemoryChunk final {
	union {
		MemoryChunk* next;
		uintptr_t reference;
	};
};

struct MetaChunk final {
	STAILQ_ENTRY(MetaChunk)		link;
	size_t 						chunkSize;
	uintptr_t 					chunkBase;
	size_t						totalSize;
	uint16_t 					chunkCount;
	uint16_t 					usedChunkCount;
	uint16_t 					firstFreeChunk;	// *some* free range
	uint16_t 					lastFreeChunk;	// inclusive
	MemoryChunk 				chunks[SLAB_SMALL_CHUNKS_PER_META_CHUNK];
	MemoryChunk* 				freeChunks;

	[[nodiscard]] MemoryArea* GetArea() const;
};

STAILQ_HEAD(MetaChunkList, MetaChunk);

struct MemoryArea final {
	STAILQ_ENTRY(MemoryArea)		link;
	VMArea* 						vmArea;
	MemoryArea *					next;
	size_t 							reserved_memory_for_mapping;
	uint16_t 						usedMetaChunkCount;
	bool 							fullyMapped;
	MetaChunk 						metaChunks[SLAB_META_CHUNKS_PER_AREA];

	[[nodiscard]] uintptr_t BaseAddress() const {
		return (uintptr_t) this - SLAB_AREA_STRUCT_OFFSET;
	}
};

STAILQ_HEAD(MemoryAreaList, MemoryArea);

struct MemoryAreaHashDefinition {
	using KeyType = uintptr_t;
	using ValueType = MemoryArea;

	static inline size_t HashKey(uintptr_t key) {
		return key / SLAB_AREA_SIZE;
	}

	static inline size_t Hash(const MemoryArea* value) {
		return HashKey(value->BaseAddress());
	}

	static inline bool Compare(uintptr_t key, const MemoryArea* value) {
		return key == value->BaseAddress();
	}

	static inline MemoryArea*& GetLink(MemoryArea* value) {
		return value->next;
	}
};

using MemoryAreaTable = BOpenHashTable<MemoryAreaHashDefinition, false, false>;

struct MemoryManager final {
	struct AllocationEntry {
		cond_var_t		condition;
		tid_t			thread;
	};

	static mutex_t					sLock;
	static rw_lock_t				sAreaTableLock;
	static MemoryAreaTable 			sAreaTable;
	static MemoryArea *				sFreeAreas;
	static int						sFreeAreaCount;
	static MetaChunkList			sFreeCompleteMetaChunks;
	static MetaChunkList			sFreeShortMetaChunks;
	static MetaChunkList			sPartialMetaChunksSmall;
	static MetaChunkList			sPartialMetaChunksMedium;
	static bool						sMaintenanceNeeded;
	static AllocationEntry*			sAllocationEntryCanWait;
	static AllocationEntry*			sAllocationEntryDontWait;
	static cond_var_t				sMaintenanceCond;

	static const size_t				kAreaAdminSize = roundup2(sizeof(MemoryArea), PAGE_SIZE);

	static void Init();

	static void Free(void * pages, uint32_t flags);
	static int AllocateRaw(size_t size, uint32_t flags, void*& _pages);
	static ObjectCache* FreeRawOrReturnCache(void* pages, uint32_t flags);
	static size_t AcceptableChunkSize(size_t size);
	static int Allocate(ObjectCache* cache, uint32_t flags, void*& _pages);
	static void PerformMaintenance();
	static void InitPostArea();

	static bool _GetChunk(MetaChunkList* metaChunkList, size_t chunkSize,
			MetaChunk*& _metaChunk, MemoryChunk*& _chunk);
	static void _PrepareMetaChunk(MetaChunk* metaChunk, size_t chunkSize);
	static void _FreeChunk(MemoryArea* area, MetaChunk* metaChunk, MemoryChunk* chunk,
				uintptr_t chunkAddress, bool alreadyUnmapped, uint32_t flags);
	static int _UnmapChunk(VMArea * vmArea, uintptr_t address, size_t size);
	static void _FreeArea(MemoryArea* area, bool areaRemoved, uint32_t flags);
	static void _PushFreeArea(MemoryArea* area);
	static MemoryArea * _PopFreeArea();
	static uintptr_t _AreaBaseAddressForAddress(uintptr_t address);
	static MemoryArea* _AreaForAddress(uintptr_t address);
	static uint32_t _ChunkIndexForAddress(const MetaChunk* metaChunk, uintptr_t address);
	static uintptr_t _ChunkAddress(const MetaChunk* metaChunk, const MemoryChunk* chunk);
	static bool _IsChunkFree(const MetaChunk* metaChunk, const MemoryChunk* chunk);
	static void _RequestMaintenance();
	static bool _GetChunks(MetaChunkList* metaChunkList, size_t chunkSize,
			uint32_t chunkCount, MetaChunk*& _metaChunk, MemoryChunk*& _chunk);
	static void _AddArea(MemoryArea* area);
	static int _AllocateArea(MemoryArea*& _area, uint32_t flags);
	static int _MapChunk(VMArea * vmArea, uintptr_t address, size_t size,
			size_t reserveAdditionalMemory, uint32_t flags);
	static int _AllocateChunks(size_t chunkSize, uint32_t chunkCount,
			uint32_t flags, MetaChunk*& _metaChunk, MemoryChunk*& _chunk);
	static ObjectCache* GetAllocationInfo(void* address, size_t& _size);
	static void _ConvertEarlyArea(MemoryArea* area);
	static void _UnmapFreeChunksEarly(MemoryArea* area);

#if defined(CONFIG_KDEBUGGER)
	static int _DumpRawAllocations(int argc, char **argv);
	static void _PrintMetaChunkTableHeader(bool printChunks);
	static void _DumpMetaChunk(MetaChunk *metaChunk, bool printChunks,
			bool printHeader);
	static int _DumpMetaChunk(int argc, char **argv);
	static void _DumpMetaChunks(const char *name, MetaChunkList &metaChunkList,
			bool printChunks);
	static int _DumpMetaChunks(int argc, char **argv);
	static int _DumpArea(int argc, char **argv);
	static int _DumpAreas(int argc, char **argv);
	static bool _IsChunkInFreeList(const MetaChunk *metaChunk,
			const MemoryChunk *chunk);
#endif

	template<typename Type> static inline Type* _pop(Type*& head)
	{
		Type* oldHead = head;
		head = head->next;
		return oldHead;
	}

	template<typename Type> static inline void _push(Type*& head, Type* object)
	{

		object->next = head;
		head = object;
	}
};

inline void MemoryManager::_PushFreeArea(MemoryArea* area)
{
	_push(sFreeAreas, area);
	sFreeAreaCount++;
}

inline MemoryArea * MemoryManager::_PopFreeArea()
{
	if (sFreeAreaCount == 0)
		return nullptr;

	sFreeAreaCount--;
	return _pop(sFreeAreas);
}


inline uintptr_t MemoryManager::_AreaBaseAddressForAddress(uintptr_t address)
{
	return rounddown((uintptr_t)address, SLAB_AREA_SIZE);
}


inline MemoryArea* MemoryManager::_AreaForAddress(uintptr_t address)
{
	return (MemoryArea *)(_AreaBaseAddressForAddress(address)
		+ SLAB_AREA_STRUCT_OFFSET);
}


inline uint32_t MemoryManager::_ChunkIndexForAddress(const MetaChunk* metaChunk, uintptr_t address)
{
	return (address - metaChunk->chunkBase) / metaChunk->chunkSize;
}


inline uintptr_t MemoryManager::_ChunkAddress(const MetaChunk* metaChunk, const MemoryChunk* chunk)
{
	return metaChunk->chunkBase
		+ (chunk - metaChunk->chunks) * metaChunk->chunkSize;
}

inline bool MemoryManager::_IsChunkFree(const MetaChunk* metaChunk, const MemoryChunk* chunk)
{
	return chunk->next == nullptr
		|| (chunk->next >= metaChunk->chunks
			&& chunk->next < metaChunk->chunks + metaChunk->chunkCount);
}

inline MemoryArea* MetaChunk::GetArea() const
{
	return MemoryManager::_AreaForAddress((uintptr_t)this);
}
#endif

__BEGIN_DECLS

void memory_manager_init(void);
void memory_manager_init_post_area(void);
void mm_perform_maintenance_thread_work(void);

__END_DECLS

#endif /* _CORE_MM_MANAGER_H_ */
