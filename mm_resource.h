

#ifndef _CORE_MM_RESOURCE_H_
#define _CORE_MM_RESOURCE_H_

#include <mm/mm.h>

/* warning levels for low resource handlers */
enum {
	B_NO_LOW_RESOURCE = 0,
	B_LOW_RESOURCE_NOTE,
	B_LOW_RESOURCE_WARNING,
	B_LOW_RESOURCE_CRITICAL,
};

enum {
	B_KERNEL_RESOURCE_PAGES			= 0x01,	/* physical pages */
	B_KERNEL_RESOURCE_MEMORY		= 0x02,	/* reservable memory */
	B_KERNEL_RESOURCE_SEMAPHORES	= 0x04,	/* semaphores */
	B_KERNEL_RESOURCE_ADDRESS_SPACE	= 0x08, /* address space */

	B_ALL_KERNEL_RESOURCES			= B_KERNEL_RESOURCE_PAGES
										| B_KERNEL_RESOURCE_MEMORY
										| B_KERNEL_RESOURCE_SEMAPHORES
										| B_KERNEL_RESOURCE_ADDRESS_SPACE
};

typedef void (*low_resource_func)(void *data, uint32_t resources, int32_t level);

__BEGIN_DECLS

void low_resource_manager_init(void);
void low_resource_manager_init_post_thread(void);
int32_t low_resource_state(uint32_t resources);
void low_resource(uint32_t resource, uint64_t requirements, uint32_t flags,
		time_t timeout);

// these calls might get public some day
int register_low_resource_handler(low_resource_func function, void *data,
		uint32_t resources, int32_t priority);
int unregister_low_resource_handler(low_resource_func function, void *data);

__END_DECLS

#endif /* _CORE_MM_RESOURCE_H_ */
