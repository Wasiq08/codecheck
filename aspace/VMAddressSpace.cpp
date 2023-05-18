#include <mm/VMAddressSpace.h>
#include <lib/OpenHashTable.h>
#include <mm/mm_allocator.h>
#include <mm/mm_cache.h>
#include <cinttypes>
#include <core/debug.h>
#include <lib/cxxabi.h>
#include <core/random.h>

#include "VMKernelAddressSpace.h"
#include "VMUserAddressSpace.h"


//#define TRACE_VM
#ifdef TRACE_VM
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


#define ASPACE_HASH_TABLE_SIZE 1024


// #pragma mark - AddressSpaceHashDefinition

struct AddressSpaceHashDefinition {
	typedef pid_t			KeyType;
	using ValueType = VMAddressSpace;

	[[nodiscard]] static size_t HashKey(pid_t key)
	{
		return boost::hash_value(key);
	}

	[[nodiscard]] static size_t Hash(const VMAddressSpace* value)
	{
		return HashKey(value->ID());
	}

	[[nodiscard]] static bool Compare(pid_t key, const VMAddressSpace* value)
	{
		return value->ID() == key;
	}

	[[nodiscard]] static VMAddressSpace*& GetLink(VMAddressSpace* value)
	{
		return value->HashTableLink();
	}
};

using AddressSpaceTable = BOpenHashTable<AddressSpaceHashDefinition, false>;

static AddressSpaceTable	sAddressSpaceTable;
static rw_lock_t			sAddressSpaceTableLock{"address spaces table"};

VMAddressSpace* VMAddressSpace::sKernelAddressSpace;
ObjectCache *VMAddressSpace::sUserAreaCache;

// #pragma mark - VMAddressSpace


VMAddressSpace::VMAddressSpace(pid_t id, uintptr_t base, size_t size,
	const char* name)
	:
	fHashTableLink(nullptr),
	fBase(base),
	fEndAddress(base + (size - 1)),
	fFreeSpace(size),
	fLock{name},
	fID(id),
	fRefCount(1),
	fFaultCount(0),
	fChangeCount(0),
	fTranslationMap(nullptr),
	fRandomizingEnabled(true),
	fDeleting(false),
	fExternalMemoryAllowed(false)
{
}


VMAddressSpace::~VMAddressSpace()
{
	TRACE(("VMAddressSpace::~VMAddressSpace: called on aspace %" PRId32 "\n",
		ID()));

	WriteLock();

	delete fTranslationMap;

	WriteUnlock();
}


/*static*/ int __init_text
VMAddressSpace::Init()
{
	// create the area and address space hash tables
	{
		int error = sAddressSpaceTable.Init(ASPACE_HASH_TABLE_SIZE);
		if (error != 0)
			panic("vm_init: error creating aspace hash table\n");
	}

	// create the initial kernel address space
	if (Create(B_SYSTEM_TEAM, VM_KERNEL_SPACE_BASE, VM_KMAP_LIMIT - VM_KERNEL_SPACE_BASE, true,
			&sKernelAddressSpace) != 0)
	{
		panic("vm_init: error creating kernel address space!\n");
	}

	sUserAreaCache = create_object_cache("VMUserArea", sizeof(VMUserArea),
			alignof(VMUserArea), nullptr, nullptr, nullptr);

	if(!sUserAreaCache)
	{
		panic("Can't create VMUserArea cache");
	}

#if defined(CONFIG_KDEBUGGER)
	add_debugger_command("aspaces", &_DumpListCommand,
		"Dump a list of all address spaces");
	add_debugger_command("aspace", &_DumpCommand,
		"Dump info about a particular address space");
#endif

	return 0;
}

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
void VMAddressSpace::ResetCompat(bool compat32)
{
	if(compat32) {
		fEndAddress = VM_PROC_LIMIT_32 - 1;
	} else {
		fEndAddress = VM_PROC_LIMIT_64 - 1;
	}
	fFreeSpace = fEndAddress - fBase + 1;
}
#endif


/*! Deletes all areas in the specified address space, and the address
	space by decreasing all reference counters. It also marks the
	address space of being in deletion state, so that no more areas
	can be created in it.
	After this, the address space is not operational anymore, but might
	still be in memory until the last reference has been released.
*/
void
VMAddressSpace::RemoveAndPut()
{
	WriteLock();
	fDeleting = true;
	WriteUnlock();

	vm_delete_areas(this, true);
	Put();
}


int
VMAddressSpace::InitObject()
{
	return 0;
}


/*static*/ int
VMAddressSpace::Create(pid_t teamID, uintptr_t base, size_t size, bool kernel,
	VMAddressSpace** _addressSpace)
{
	VMAddressSpace* addressSpace = kernel
		? (VMAddressSpace*)new(std::nothrow) VMKernelAddressSpace(teamID, base,
			size)
		: (VMAddressSpace*)new(std::nothrow) VMUserAddressSpace(teamID, base,
			size);
	if (addressSpace == nullptr)
		return -ENOMEM;

	int status = addressSpace->InitObject();
	if (status != 0) {
		delete addressSpace;
		return status;
	}

	TRACE(("VMAddressSpace::Create(): team %" PRId32 " (%skernel): %#lx "
		"bytes starting at %#lx => %p\n", teamID, kernel ? "" : "!", size,
		base, addressSpace));

	// create the corresponding translation map
	status = arch_vm_translation_map_create_map(kernel,
		&addressSpace->fTranslationMap);
	if (status != 0) {
		delete addressSpace;
		return status;
	}

	// Assign team ID to the translation map, so the underlying
	// arch specific code can make use of it
	addressSpace->fTranslationMap->SetMapId(teamID);

	// add the aspace to the global hash table
	rw_lock_write_lock(&sAddressSpaceTableLock);
	sAddressSpaceTable.InsertUnchecked(addressSpace);
	rw_lock_write_unlock(&sAddressSpaceTableLock);

	*_addressSpace = addressSpace;
	return 0;
}


/*static*/ VMAddressSpace*
VMAddressSpace::GetKernel()
{
	// we can treat this one a little differently since it can't be deleted
	sKernelAddressSpace->Get();
	return sKernelAddressSpace;
}


/*static*/ pid_t
VMAddressSpace::CurrentID()
{
	struct thread* thread = thread_get_current_thread();

	if (thread != nullptr && thread->task->addressSpace != nullptr)
		return thread->task->id;

	return -ENXIO;
}


/*static*/ VMAddressSpace*
VMAddressSpace::GetCurrent()
{
	struct thread* thread = thread_get_current_thread();

	if (thread != nullptr) {
		VMAddressSpace* addressSpace = thread->task->addressSpace;
		if (addressSpace != nullptr) {
			addressSpace->Get();
			return addressSpace;
		}
	}

	return nullptr;
}


/*static*/ VMAddressSpace*
VMAddressSpace::Get(pid_t teamID)
{
	rw_lock_read_lock(&sAddressSpaceTableLock);
	VMAddressSpace* addressSpace = sAddressSpaceTable.Lookup(teamID);
	if (addressSpace)
		addressSpace->Get();
	rw_lock_read_unlock(&sAddressSpaceTableLock);

	return addressSpace;
}


/*static*/ VMAddressSpace*
VMAddressSpace::DebugFirst()
{
	return sAddressSpaceTable.GetIterator().Next();
}


/*static*/ VMAddressSpace*
VMAddressSpace::DebugNext(VMAddressSpace* addressSpace)
{
	if (addressSpace == nullptr)
		return nullptr;

	AddressSpaceTable::Iterator it
		= sAddressSpaceTable.GetIterator(addressSpace->ID());
	it.Next();
	return it.Next();
}


/*static*/ VMAddressSpace*
VMAddressSpace::DebugGet(pid_t teamID)
{
	return sAddressSpaceTable.Lookup(teamID);
}


/*static*/ void
VMAddressSpace::_DeleteIfUnreferenced(pid_t id)
{
	rw_lock_write_lock(&sAddressSpaceTableLock);

	bool remove = false;
	VMAddressSpace* addressSpace = sAddressSpaceTable.Lookup(id);
	if (addressSpace != nullptr && addressSpace->RefCount() == 0) {
		sAddressSpaceTable.RemoveUnchecked(addressSpace);
		remove = true;
	}

	rw_lock_write_unlock(&sAddressSpaceTableLock);

	if (remove)
		delete addressSpace;
}

bool
VMAddressSpace::_IsRandomized(uint32_t addressSpec) const
{
	return fRandomizingEnabled
		&& (addressSpec == B_RANDOMIZED_ANY_ADDRESS
			|| addressSpec == B_RANDOMIZED_BASE_ADDRESS);
}


uintptr_t
VMAddressSpace::_RandomizeAddress(uintptr_t start, uintptr_t end,
	size_t alignment, bool initial) const noexcept
{
	ASSERT((start & uintptr_t(alignment - 1)) == 0);
	ASSERT(start <= end);

	if (start == end)
		return start;

	uintptr_t range = end - start + 1;
	if (initial)
		range = std::min(range, max_initial_randomize());
	else
		range = std::min(range, max_randomize());

	uintptr_t random;

	arc4random_buf(&random, sizeof(random));

	random %= range;
	random &= ~uintptr_t(alignment - 1);

	return start + random;
}


#if defined(CONFIG_KDEBUGGER)
/*static*/ int
VMAddressSpace::_DumpCommand(int argc, char** argv)
{
	VMAddressSpace* aspace;

	if (argc < 2) {
		kprintf("aspace: not enough arguments\n");
		return 0;
	}

	// if the argument looks like a number, treat it as such

	{
		pid_t id = strtoul(argv[1], nullptr, 0);

		aspace = sAddressSpaceTable.Lookup(id);
		if (aspace == nullptr) {
			kprintf("invalid aspace id\n");
		} else {
			aspace->Dump();
		}
		return 0;
	}
	return 0;
}


/*static*/ int
VMAddressSpace::_DumpListCommand([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
	kprintf("  %*s      id     %*s     %*s   area count    area size\n",
		B_PRINTF_POINTER_WIDTH, "address", B_PRINTF_POINTER_WIDTH, "base",
		B_PRINTF_POINTER_WIDTH, "end");

	AddressSpaceTable::Iterator it = sAddressSpaceTable.GetIterator();
	while (VMAddressSpace* space = it.Next()) {
		int32_t areaCount = 0;
		size_t areaSize = 0;
		for (VMAddressSpace::AreaIterator areaIt = space->GetAreaIterator();
				VMArea* area = areaIt.Next();) {
			areaCount++;
			areaSize += area->Size();
		}
		kprintf("%p  %6" PRId32 "   %#010" PRIxPTR "   %#10" PRIxPTR
			"   %10" PRId32 "   %10zd\n", space, space->ID(),
			space->Base(), space->EndAddress(), areaCount, areaSize);
	}

	return 0;
}
#endif

void
VMAddressSpace::Dump()
{
#if defined(CONFIG_KDEBUGGER)
	kprintf("dump of address space at %p:\n", this);
	kprintf("id: %" PRId32 "\n", fID);
	kprintf("ref_count: %" PRId32 "\n", RefCount());
	kprintf("fault_count: %" PRId32 "\n", FaultCount());
	kprintf("translation_map: %p\n", fTranslationMap);
	kprintf("base: %#" PRIxPTR "\n", fBase);
	kprintf("end: %#" PRIxPTR "\n", fEndAddress);
	kprintf("change_count: %" PRId32 "\n", fChangeCount);
#endif
}

