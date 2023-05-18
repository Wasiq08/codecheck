
#ifndef __MM_LOW_MEM_FRAMEWORK__
#define __MM_LOW_MEM_FRAMEWORK__

#include <mm/mm_resource.h>

__BEGIN_DECLS

void pagefault_oom_kill();
bool oom_kill_page_alloc_retry(bool killable);

__END_DECLS

#endif /*!__MM_LOW_MEM_FRAMEWORK__*/
