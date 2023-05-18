#ifndef VM_PMAP_H
#define VM_PMAP_H

#include <mm/page.h>

__BEGIN_DECLS

vm_paddr_t pmap_kernel_virt_to_phys(const void * ptr);

__END_DECLS

#endif // VM_PMAP_H
