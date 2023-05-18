
#ifndef _KERNEL_CORE_FS_CACHE_H_
#define _KERNEL_CORE_FS_CACHE_H_

#include <fs/vfs.h>

__BEGIN_DECLS

void file_cache_init(void);

void * file_cache_create(fs_id mountID, vnode_id vnodeID, off_t size, bool temporaryCache);
void file_cache_delete(void* _cacheRef);
int file_cache_set_size(void* _cacheRef, off_t newSize);
int file_cache_sync(void* _cacheRef);
int file_cache_read(void* _cacheRef, void * cookie, off_t offset, void* buffer, size_t* _size);
int file_cache_write(void* _cacheRef, void * cookie, off_t offset, const void* buffer, size_t* _size);
void file_cache_enable(void* _cacheRef);
int file_cache_disable(void* _cacheRef);
vm_paddr_t file_cache_empty_zero_page(void);

__END_DECLS

#endif /* _KERNEL_CORE_FS_CACHE_H_ */
