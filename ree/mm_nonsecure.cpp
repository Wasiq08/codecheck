
#include <core/error.h>
#include <core/cpu.h>
#include <mm/xmalloc.h>
#include <mm/mm.h>
#include <core/machine_desc.h>
#include <lib/string.h>
#include <core/module.h>
#include <core/debug.h>
#include <core/monitor-new.h>
#include <trustware/vm.h>
#include <trustwareos/private/wsm.h>
#include <core/thread.h>

#if !defined(CONFIG_NO_TRUSTZONE)
void *kva_map_nonsecure_memory_to_kernel(uint64_t virt, size_t length)
{
	ns_vaddr_t virt_base = ROUND_PGDOWN_LPAE(virt);
	size_t virt_size = ROUND_PGUP_LPAE(virt + length) - virt_base;
	off_t virt_off = virt & (PAGE_SIZE - 1);

	vm_pindex_t * pfns;
	const size_t num_pfn = virt_size >> PAGE_SHIFT;
	size_t i, j;

	{
		irqstate_t flags = irqsave();
		struct cpu_data * ci = cpu_local_data();
		if(ci->curthread->task != team_get_kernel_team()) {
			irqrestore(flags);
			printk(KERN_ERR|WITHUART, "This API must not be called outside kernel thread context\n");
			return nullptr;
		}
		irqrestore(flags);
	}

	if (num_pfn < 1 || num_pfn > (NS_PAGES_PER_LEVEL * NS_PAGES_PER_LEVEL)) {
		printk(KERN_ERR, "Pfn limit exceeded (%zu)\n", num_pfn);
		return nullptr;
	}

	pfns = (vm_pindex_t *)malloc_etc(sizeof(vm_pindex_t) * num_pfn, CACHE_DONT_WAIT_FOR_MEMORY | CACHE_CLEAR_MEMORY | CACHE_USE_RESERVE);

	if(pfns == nullptr) {
		return nullptr;
	}

	for(i = 0 ; i < num_pfn ; ++i) {
		lpae_addr_t phys = monitor_translate_ns_address(virt_base, 1);

		virt_base += PAGE_SIZE;

		if(phys == (lpae_addr_t)-1LL) {
			free(pfns);
			return nullptr;
		}

		//Check if phys is not in secure RAM
		if (phys > TheMachine->secure_ram.base &&
				phys < TheMachine->secure_ram.base + TheMachine->secure_ram.size)
		{
			printk(KERN_ERROR, "attempt to map secure RAM address as nonsecure detected\n");
			free(pfns);
			return nullptr;
		}
		if(TheMachine->secure_ram_ext) {
			for(j=0; TheMachine->secure_ram_ext[j].size; j++) {
				if (phys > TheMachine->secure_ram_ext[j].base &&
						phys < TheMachine->secure_ram_ext[j].base + TheMachine->secure_ram_ext[j].size)
				{
					printk(KERN_ERROR, "attempt to map secure RAM ext address as nonsecure detected\n");
					free(pfns);
					return nullptr;
				}
			}
		}

		pfns[i] = phys >> PAGE_SHIFT;
	}

	void * ptr = kva_map_pfns(pfns, num_pfn, VMA_TYPE_S_WSM);

	free(pfns);

	if(ptr) {
		ptr = (void *)(((char *)ptr) + virt_off);
	}

	return ptr;
}
#endif
