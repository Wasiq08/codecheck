
#include <core/debug.h>
#include <mm/mm_resource.h>
#include <core/machine_desc.h>
#include <lib/DoublyLinkedList.h>
#include <lib/cxxabi.h>
#include <trustware/sem.h>
#include <ipc/sem.h>
#include <lib/AutoLock.h>
#include <cinttypes>

struct low_resource_handler
		: public DoublyLinkedListLinkImpl<low_resource_handler> {
	low_resource_func	function;
	void*				data;
	uint32_t			resources;
	int32_t				priority;
};

typedef DoublyLinkedList<low_resource_handler> HandlerList;

// page limits
static const size_t kNotePagesLimit = VM_PAGE_RESERVE_USER * 4;
static const size_t kWarnPagesLimit = VM_PAGE_RESERVE_USER;
static const size_t kCriticalPagesLimit = VM_PAGE_RESERVE_SYSTEM;

// memory limits
static const off_t kMinNoteMemoryLimit		= VM_MEMORY_RESERVE_USER * 4;
static const off_t kMinWarnMemoryLimit		= VM_MEMORY_RESERVE_USER;
static const off_t kMinCriticalMemoryLimit	= VM_MEMORY_RESERVE_SYSTEM;
static off_t sNoteMemoryLimit;
static off_t sWarnMemoryLimit;
static off_t sCriticalMemoryLimit;

// address space limits
static const size_t kMinNoteSpaceLimit		= 128 * 1024 * 1024;
static const size_t kMinWarnSpaceLimit		= 64 * 1024 * 1024;
static const size_t kMinCriticalSpaceLimit	= 32 * 1024 * 1024;

static int32_t sLowPagesState = B_NO_LOW_RESOURCE;
static int32_t sLowMemoryState = B_NO_LOW_RESOURCE;
static int32_t sLowSemaphoresState = B_NO_LOW_RESOURCE;
static int32_t sLowSpaceState = B_NO_LOW_RESOURCE;
static uint32_t sLowResources = 0;	// resources that are not B_NO_LOW_RESOURCE

static recursive_mutex_t sLowResourceLock = RECURSIVE_MUTEX_INITIALIZER("low resource");
static sem_id sLowResourceWaitSem;
static HandlerList sLowResourceHandlers;
static cond_var_t sLowResourceWaiterCondition{"low resource"};
static time_t sLastMeasurement;

#if defined(CONFIG_NO_TRUSTZONE)
static const time_t kLowResourceInterval = SECONDS(3);
static const time_t kWarnResourceInterval = MILLISECS(500);
static const time_t kCriticalResourceInterval = MILLISECS(500);
#else
static const time_t kLowResourceInterval = SECONDS(5);
static const time_t kWarnResourceInterval = MILLISECS(1000);
static const time_t kCriticalResourceInterval = MILLISECS(500);
#endif

static const char*
state_to_string(uint32_t state)
{
	switch (state) {
		case B_LOW_RESOURCE_CRITICAL:
			return "critical";
		case B_LOW_RESOURCE_WARNING:
			return "warning";
		case B_LOW_RESOURCE_NOTE:
			return "note";

		default:
			return "normal";
	}
}

static int32_t
low_resource_state_no_update(uint32_t resources)
{
	int32_t state = B_NO_LOW_RESOURCE;

	if ((resources & B_KERNEL_RESOURCE_PAGES) != 0)
		state = MAX(state, sLowPagesState);
	if ((resources & B_KERNEL_RESOURCE_MEMORY) != 0)
		state = MAX(state, sLowMemoryState);
	if ((resources & B_KERNEL_RESOURCE_SEMAPHORES) != 0)
		state = MAX(state, sLowSemaphoresState);
	if ((resources & B_KERNEL_RESOURCE_ADDRESS_SPACE) != 0)
		state = MAX(state, sLowSpaceState);

	return state;
}


/*!	Calls low resource handlers for the given resources.
	sLowResourceLock must be held.
*/
static void call_handlers(uint32_t lowResources)
{
	if (sLowResourceHandlers.IsEmpty())
		return;

	// Add a marker, so we can drop the lock while calling the handlers and
	// still iterate safely.
	low_resource_handler marker{};
	sLowResourceHandlers.Insert(&marker, false);

	while (low_resource_handler* handler
			= sLowResourceHandlers.GetNext(&marker)) {
		// swap with handler
		sLowResourceHandlers.Swap(&marker, handler);
		marker.priority = handler->priority;

		uint32_t resources = handler->resources & lowResources;
		if (resources != 0) {
			recursive_mutex_unlock(&sLowResourceLock);
			handler->function(handler->data, resources,
				low_resource_state_no_update(resources));
			recursive_mutex_lock(&sLowResourceLock);
		}
	}

	// remove marker
	sLowResourceHandlers.Remove(&marker);
}

static void compute_state()
{
	sLastMeasurement = system_time();

	sLowResources = B_ALL_KERNEL_RESOURCES;

	// free pages state
	uint32_t freePages = vm_number_of_free_pages();

	int32_t oldState = sLowPagesState;
	if (freePages < kCriticalPagesLimit) {
		sLowPagesState = B_LOW_RESOURCE_CRITICAL;
	} else if (freePages < kWarnPagesLimit) {
		sLowPagesState = B_LOW_RESOURCE_WARNING;
	} else if (freePages < kNotePagesLimit) {
		sLowPagesState = B_LOW_RESOURCE_NOTE;
	} else {
		sLowPagesState = B_NO_LOW_RESOURCE;
		sLowResources &= ~B_KERNEL_RESOURCE_PAGES;
	}

	if (sLowPagesState != oldState) {
		printk(KERN_INFO, "low resource pages: %s -> %s\n", state_to_string(oldState),
			state_to_string(sLowPagesState));
	}

	// free memory state
	off_t freeMemory = vm_available_not_needed_memory();

	oldState = sLowMemoryState;
	if (freeMemory < sCriticalMemoryLimit) {
		sLowMemoryState = B_LOW_RESOURCE_CRITICAL;
	} else if (freeMemory < sWarnMemoryLimit) {
		sLowMemoryState = B_LOW_RESOURCE_WARNING;
	} else if (freeMemory < sNoteMemoryLimit) {
		sLowMemoryState = B_LOW_RESOURCE_NOTE;
	} else {
		sLowMemoryState = B_NO_LOW_RESOURCE;
		sLowResources &= ~B_KERNEL_RESOURCE_MEMORY;
	}

	if (sLowMemoryState != oldState) {
		printk(KERN_INFO, "low resource memory: %s -> %s\n", state_to_string(oldState),
			state_to_string(sLowMemoryState));
	}

	// free semaphores state
	uint32_t maxSems = sem_max_sems();
	uint32_t freeSems = maxSems - sem_used_sems();

	oldState = sLowSemaphoresState;
	if (freeSems < (maxSems >> 16)) {
		sLowSemaphoresState = B_LOW_RESOURCE_CRITICAL;
	} else if (freeSems < (maxSems >> 8)) {
		sLowSemaphoresState = B_LOW_RESOURCE_WARNING;
	} else if (freeSems < (maxSems >> 4)) {
		sLowSemaphoresState = B_LOW_RESOURCE_NOTE;
	} else {
		sLowSemaphoresState = B_NO_LOW_RESOURCE;
		sLowResources &= ~B_KERNEL_RESOURCE_SEMAPHORES;
	}

	if (sLowSemaphoresState != oldState) {
		printk(KERN_INFO, "low resource semaphores: %s -> %s\n",
			state_to_string(oldState), state_to_string(sLowSemaphoresState));
	}

	// free kernel address space state
	// TODO: this should take fragmentation into account
	size_t freeSpace = vm_kernel_address_space_left();

	oldState = sLowSpaceState;
	if (freeSpace < kMinCriticalSpaceLimit) {
		sLowSpaceState = B_LOW_RESOURCE_CRITICAL;
	} else if (freeSpace < kMinWarnSpaceLimit) {
		sLowSpaceState = B_LOW_RESOURCE_WARNING;
	} else if (freeSpace < kMinNoteSpaceLimit) {
		sLowSpaceState = B_LOW_RESOURCE_NOTE;
	} else {
		sLowSpaceState = B_NO_LOW_RESOURCE;
		sLowResources &= ~B_KERNEL_RESOURCE_ADDRESS_SPACE;
	}

	if (sLowSpaceState != oldState) {
		printk(KERN_INFO, "low resource address space: %s -> %s\n",
			state_to_string(oldState), state_to_string(sLowSpaceState));
	}
}

[[noreturn]] static int low_resource_manager(void*)
{
	time_t timeout = kLowResourceInterval;

	while (true) {
		acquire_sem_etc(sLowResourceWaitSem, 1, B_RELATIVE_TIMEOUT,
			timeout);

		RecursiveLocker _(&sLowResourceLock);

		compute_state();
		auto state = low_resource_state_no_update(B_ALL_KERNEL_RESOURCES);

		printk(KERN_DEBUG, "low_resource_manager: state = %d, %d free pages, %" B_PRIdOFF " free "
			"memory, %u free semaphores\n", state, vm_number_of_free_pages(),
			vm_available_not_needed_memory(),
			sem_max_sems() - sem_used_sems());

		if (state < B_LOW_RESOURCE_NOTE)
			continue;

		call_handlers(sLowResources);

		if (state == B_LOW_RESOURCE_CRITICAL)
			timeout = kCriticalResourceInterval;
		else if (state == B_LOW_RESOURCE_WARNING)
			timeout = kWarnResourceInterval;
		else
			timeout = kLowResourceInterval;

		sLowResourceWaiterCondition.NotifyAll();
	}
}

/*!	Notifies the low resource manager that a resource is lacking. If \a flags
	and \a timeout specify a timeout, the function will wait until the low
	resource manager has finished its next iteration of calling low resource
	handlers, or until the timeout occurs (whichever happens first).
*/
void
low_resource(uint32_t, uint64_t, uint32_t flags, time_t timeout)
{
	release_sem(sLowResourceWaitSem);

	if ((flags & B_RELATIVE_TIMEOUT) == 0 || timeout > 0) {
		sLowResourceWaiterCondition.Wait(flags, timeout);
	}
}

int32_t
low_resource_state(uint32_t resources)
{
	RecursiveLocker locker(sLowResourceLock);

	if (system_time() - sLastMeasurement > MILLISECS(500))
		compute_state();

	int32_t state = low_resource_state_no_update(resources);

	return state;
}

#if defined(CONFIG_KDEBUGGER)
static int
dump_handlers(int, char**)
{
	kprintf("current state: %c%c%c%c\n",
		(sLowResources & B_KERNEL_RESOURCE_PAGES) != 0 ? 'p' : '-',
		(sLowResources & B_KERNEL_RESOURCE_MEMORY) != 0 ? 'm' : '-',
		(sLowResources & B_KERNEL_RESOURCE_SEMAPHORES) != 0 ? 's' : '-',
		(sLowResources & B_KERNEL_RESOURCE_ADDRESS_SPACE) != 0 ? 'a' : '-');
	kprintf("  pages:  %s (%" PRIu64 ")\n", state_to_string(sLowPagesState),
		(uint64_t)vm_number_of_free_pages());
	kprintf("  memory: %s (%" B_PRIdOFF ")\n", state_to_string(sLowMemoryState),
		vm_available_not_needed_memory_debug());
	kprintf("  sems:   %s (%" PRIu32 ")\n",
		state_to_string(sLowSemaphoresState), sem_max_sems() - sem_used_sems());
	kprintf("  aspace: %s (%zu)\n\n",
		state_to_string(sLowSpaceState), vm_kernel_address_space_left());

	HandlerList::Iterator iterator = sLowResourceHandlers.GetIterator();
	kprintf("function    data         resources  prio  function-name\n");

	while (iterator.HasNext()) {
		low_resource_handler *handler = iterator.Next();

		const char* symbol = nullptr;
		elf_debug_lookup_symbol_address((uintptr_t)handler->function, nullptr,
			&symbol, nullptr, nullptr, nullptr);

		char resources[16];
		snprintf(resources, sizeof(resources), "%c %c %c %c",
			handler->resources & B_KERNEL_RESOURCE_PAGES ? 'p' : ' ',
			handler->resources & B_KERNEL_RESOURCE_MEMORY ? 'm' : ' ',
			handler->resources & B_KERNEL_RESOURCE_SEMAPHORES ? 's' : ' ',
			handler->resources & B_KERNEL_RESOURCE_ADDRESS_SPACE ? 'a' : ' ');

		kprintf("%p  %p   %s      %4" PRId32 "  %s\n", handler->function,
			handler->data, resources, handler->priority, symbol);
	}

	return 0;
}
#endif

void __init_text low_resource_manager_init(void)
{
	// compute the free memory limits
	off_t totalMemory = vm_page_num_pages() * PAGE_SIZE;

	sNoteMemoryLimit = totalMemory / 16;
	if (sNoteMemoryLimit < kMinNoteMemoryLimit) {
		sNoteMemoryLimit = kMinNoteMemoryLimit;
		sWarnMemoryLimit = kMinWarnMemoryLimit;
		sCriticalMemoryLimit = kMinCriticalMemoryLimit;
	} else {
		sWarnMemoryLimit = totalMemory / 64;
		sCriticalMemoryLimit = totalMemory / 256;
	}

#if defined(CONFIG_KDEBUGGER)
	add_debugger_command("low_resource", &dump_handlers,
		"Dump list of low resource handlers");
#endif
}

void __init_text low_resource_manager_init_post_thread(void)
{
	sLowResourceWaitSem = create_sem(0, "low resource wait");
	tid_t id = spawn_kernel_thread_etc(low_resource_manager,
			"low resource manager", K_LOW_PRIORITY, nullptr, 0);
	if(id < 0)
		panic("Can't spawn low resource manager thread");
	resume_thread(id);
}

int unregister_low_resource_handler(low_resource_func function, void* data)
{
	RecursiveLocker locker(&sLowResourceLock);
	HandlerList::Iterator iterator = sLowResourceHandlers.GetIterator();

	while (iterator.HasNext()) {
		low_resource_handler* handler = iterator.Next();

		if (handler->function == function && handler->data == data) {
			sLowResourceHandlers.Remove(handler);
			free(handler);
			return 0;
		}
	}

	return -ENOENT;
}

int register_low_resource_handler(low_resource_func function, void* data, uint32_t resources, int32_t priority) {
	auto *newHandler = (low_resource_handler*)malloc(
		sizeof(low_resource_handler));
	if (newHandler == nullptr)
		return -ENOMEM;

	newHandler->function = function;
	newHandler->data = data;
	newHandler->resources = resources;
	newHandler->priority = priority;

	RecursiveLocker locker(&sLowResourceLock);

	// sort it in after priority (higher priority comes first)

	HandlerList::ReverseIterator iterator
		= sLowResourceHandlers.GetReverseIterator();
	low_resource_handler* last = nullptr;
	while (iterator.HasNext()) {
		low_resource_handler *handler = iterator.Next();

		if (handler->priority >= priority) {
			sLowResourceHandlers.Insert(last, newHandler);
			return 0;
		}
		last = handler;
	}

	sLowResourceHandlers.Add(newHandler, false);
	return 0;
}
