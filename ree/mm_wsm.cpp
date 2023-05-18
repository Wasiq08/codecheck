

#include <core/error.h>
#include <core/cpu.h>
#include <core/sched.h>
#include <mm/xmalloc.h>
#include <mm/mm.h>
#include <core/task.h>
#include <core/context.h>
#include <core/usercopy.h>
#include <core/thread.h>
#include <core/scm.h>
#include <core/io.h>
#include <fs/vfs.h>
#include <core/linkage.h>
#include <core/wsm.h>
#include <core/debug.h>
#include <core/monitor-new.h>
#include <mm/mm_locking.h>
#include <lib/string.h>
#include <lib/ds.h>
#include <lib/cxxabi.h>
#include <core/procstat.h>
#include <mm/VMAddressSpace.h>
#include <core/listeners.h>

#include <trustwareos/private/wsm.h>
#include <trustwareos/private/tzrsrc_msg.h>
#include <trustwareos/private/nsrpc_dump.h>
#include <trustware/vm.h>
#include <trustware/ree_shmem.h>

#include <lib/AutoLock.h>
#include <lib/OpenHashTable.h>
#include <lib/Portability.h>
#include <lib/syscalls.h>

struct WsmNodeHashTableDefinition {
	typedef int KeyType;
	using ValueType = wsm_node;

	[[nodiscard]] size_t HashKey(KeyType key) const {
		return key * 37;
	}

	size_t Hash(ValueType *value) const {
		return HashKey(value->id);
	}

	bool Compare(KeyType key, ValueType *value) const {
		return value->id == key;
	}

	ValueType *&GetLink(ValueType *value) const {
		return value->hash_next;
	}
};

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
struct ree_shmem_info_compat32 {
	int32_t id;
	uint32_t tee_ctx_id;
	int64_t serial_number;
	uint32_t size;
};

template<>
struct __hidden CompatTypeConverter<ree_shmem_info> {
	typedef ree_shmem_info_compat32 compat_type;

	static void FromUser(ree_shmem_info &nativeData, const compat_type &userData) {
		nativeData.id = userData.id;
		nativeData.tee_ctx_id = userData.tee_ctx_id;
		nativeData.serial_number = userData.serial_number;
		nativeData.size = userData.size;
	}

	static void ToUser(compat_type &userData, const ree_shmem_info &nativeData) {
		userData.id = nativeData.id;
		userData.tee_ctx_id = nativeData.tee_ctx_id;
		userData.serial_number = nativeData.serial_number;
		userData.size = nativeData.size;
	}
};
#endif

static BOpenHashTable<WsmNodeHashTableDefinition, false, false> sWSMHash;
static smp_rwspinlock wsm_node_rwlock{"wsm node hash lock"};
static int sNextWSMId = 1;
static int64_t sNextWSMSerialNumber = 1;

static ObjectCache *wsm_pool;

void __init_text wsm_init(void) {
	wsm_pool = create_object_cache("wsm_node", sizeof(wsm_node), 0, nullptr, nullptr, nullptr);
	if (wsm_pool == nullptr) {
		panic("Can't create wsm node cache");
	}
	if (object_cache_set_minimum_reserve(wsm_pool, 16) < 0) {
		panic("Can't set minimum wsm node reserve");
	}
	if (object_cache_reserve(wsm_pool, 16, 0) < 0) {
		panic("Can't reserve wsm pool nodes");
	}
	if (sWSMHash.Init(128) < 0) {
		panic("Can't resize WSM hash");
	}
}

wsm_node::~wsm_node() {
	if (kernel_map_area >= 0) {
		delete_area(kernel_map_area);
	}

	free(pfns);
}

void wsm_node::LastReferenceReleased() {
	object_cache_delete(wsm_pool, this);
}

wsm_node_ref wsm_lookup_node(int id) {
	InterruptsSmpReadSpinLocker locker(wsm_node_rwlock);
	return wsm_node_ref(sWSMHash.Lookup(id));
}

wsm_node_ref wsm_translate_node(int memid) {
	auto node = wsm_lookup_node(memid);

	if (!node) {
		printk(KERN_ERROR, "Invalid MEMID: %d\n", memid);
		return {};
	}

	if (!(node->type & NS_WSM_FLAG_KERNEL)) {
		printk(KERN_ERROR, "Invalid channel: %d\n", memid);
		return {};
	}

	return node;
}

int phys_wsm_register_node(uint32_t args_pfn_lo, [[maybe_unused]] uint32_t args_pfn_hi) {
	vm_pindex_t pfn = 0;
	struct ns_level_registration *level0;
	struct ns_level_registration *level1;
	size_t l0_index, j, pfn_index;
	wsm_node_ref node;
	uint32_t num_pfn;

	pfn = args_pfn_lo;

	level0 = (struct ns_level_registration *) kva_get_nonsecure_remap_slot(0, (vm_paddr_t) pfn << PAGE_SHIFT);

	static_assert(sizeof(num_pfn) == sizeof(level0->num_pfns), "types sizes must match");

	if (memcpy_user(&num_pfn, &level0->num_pfns, sizeof(num_pfn)) < 0) {
		return -EFAULT;
	}

	if (num_pfn < 1 || num_pfn > (NS_PAGES_PER_LEVEL * NS_PAGES_PER_LEVEL)) {
		printk(KERN_ERR | WITHUART, "Pfn limit exceeded (%u)\n", num_pfn);
		return -EINVAL;
	}

	auto nodePtr = new(wsm_pool, CACHE_DONT_WAIT_FOR_MEMORY) wsm_node();

	if (nodePtr == nullptr) {
		printk(KERN_ERR, "Unable to allocate new WSM node\n");
		return -ENOMEM;
	}

	node.SetTo(nodePtr, true);

	node->pfns =
			(vm_pindex_t *) malloc_etc(sizeof(vm_pindex_t) * num_pfn, CACHE_DONT_WAIT_FOR_MEMORY | CACHE_CLEAR_MEMORY);
	node->type = level0->flags;
	node->size = num_pfn << PAGE_SHIFT;
	node->tee_ctx_id = level0->ctx_id;

	if (!node->pfns) {
		printk(KERN_ERR | WITHUART, "Can't allocate pfn array for WSM\n");
		return -ENOMEM;
	}

	pfn_index = 0;
	l0_index = 0;

	while (num_pfn > 0) {
		if (l0_index >= NS_PAGES_PER_LEVEL) {
			printk(KERN_ERR | WITHUART, "Pfn limit per level exceeded (%zd)\n", l0_index);
			return -EFAULT;
		}

		vm_paddr_t phys = level0->address[l0_index];

		if (!(phys & NS_PHYS_ADDR_IS_LEVEL_1)) {
			--num_pfn;
			node->pfns[pfn_index++] = phys >> PAGE_SHIFT;
		} else {
			vm_pindex_t l1_pfn = phys >> PAGE_SHIFT;
			uint32_t l1_num_pfn;

			level1 =
					(struct ns_level_registration *) kva_get_nonsecure_remap_slot(1, (vm_paddr_t) l1_pfn << PAGE_SHIFT);

			static_assert(sizeof(l1_num_pfn) == sizeof(level1->num_pfns), "types sizes must match");

			if (memcpy_user(&l1_num_pfn, &level1->num_pfns, sizeof(l1_num_pfn)) < 0) {
				return -EFAULT;
			}

			if (l1_num_pfn > NS_PAGES_PER_LEVEL || l1_num_pfn > num_pfn) {
				return -EFAULT;
			}

			for (j = 0; j < l1_num_pfn; ++j) {
				vm_paddr_t l2_addr = level1->address[j];
				if (l2_addr & NS_PHYS_ADDR_IS_LEVEL_1) {
					return -EFAULT;
				}

				node->pfns[pfn_index++] = l2_addr >> PAGE_SHIFT;
			}

			num_pfn -= l1_num_pfn;
		}
		++l0_index;
	}

	if (node->type & NS_WSM_FLAG_KERNEL) {
		node->kernel_map_area = vm_map_physical_memory_pfns(VMAddressSpace::KernelID(),
		                                                    "wsm map",
		                                                    (vaddr_t *) &node->map,
		                                                    B_RANDOMIZED_ANY_ADDRESS,
		                                                    node->size >> PAGE_SHIFT,
		                                                    VMA_TYPE_S_WSM,
		                                                    node->pfns);

		if (node->kernel_map_area < 0) {
			printk(KERN_ERR | NOPREFIX | WITHUART, "Can't create kernel WSM map (%d)\n", node->kernel_map_area);
			return node->kernel_map_area;
		}
	}

	int return_id;

	{
		InterruptsSmpWriteSpinLocker locker(wsm_node_rwlock);
		++sNextWSMId;
		if (sNextWSMId > MAX_WSM_ID) {
			sNextWSMId = MIN_WSM_ID;
		}
		while (sWSMHash.Lookup(sNextWSMId)) {
			++sNextWSMId;
			if (sNextWSMId > MAX_WSM_ID) {
				sNextWSMId = MIN_WSM_ID;
			}
		}
		node->id = sNextWSMId;
		node->serial_number = sNextWSMSerialNumber++;
		sWSMHash.InsertUnchecked(node.Get());
		return_id = node->id;

		// Hash table owns the reference now
		node.Detach();
	}

	return return_id;
}

int wsm_unregister_node(int id) {
	InterruptsSmpWriteSpinLocker locker(wsm_node_rwlock);
	wsm_node *node = sWSMHash.Lookup(id);
	if (!node) {
		printk(KERN_ERROR, "Unknown WSM id(%d)\n", id);
		return -EINVAL;
	}
	sWSMHash.RemoveUnchecked(node);
	node->id = -1;
	locker.Unlock();
	node->ReleaseReference();
	return 0;
}

#if defined(CONFIG_DEBUG)
int wsm_dump_info(void *buf, int *wsm_count, int *page_count) {
	rsrc_info_wsm *node_info = (rsrc_info_wsm *) buf;
	rsrc_info_wsm_page *page_info = nullptr;

	unsigned int index = 0;
	int total_wsm_count = 0;
	int total_page_count = 0;
	int page_cnt = 0;
	int i;

	InterruptsSmpReadSpinLocker locker(wsm_node_rwlock);

	auto it = sWSMHash.GetIterator();
	while (it.HasNext()) {
		wsm_node *node = it.Next();

		node_info->id = node->id;
		node_info->size = (node->size >> 10);
		node_info->page_cnt = (node->size >> PAGE_SHIFT);
		node_info->type = node->type;
		node_info->tee_ctx_id = node->tee_ctx_id;
		node_info->index = index++;

		page_cnt = node_info->page_cnt;

		node_info++;
		page_info = (rsrc_info_wsm_page *) node_info;

		for (i = 0; i < page_cnt; i++) {
			page_info->paddr = (((uint64_t) node->pfns[i]) << PAGE_SHIFT);
			page_info++;
		}
		node_info = (rsrc_info_wsm *) page_info;

		total_wsm_count++;
		total_page_count += page_cnt;
	}

	*wsm_count = total_wsm_count;
	*page_count = total_page_count;

	return 0;
}
#endif

int _ree_shmem_get_info(int wsm_id, ree_shmem_info *info, size_t info_size) {
	if (!info) {
		return -EINVAL;
	}
	if (info_size != sizeof(ree_shmem_info)) {
		return -EINVAL;
	}

	auto node = wsm_lookup_node(wsm_id);

	if (!node) {
		return -EBADF;
	}

	info->id = node->id;
	info->serial_number = node->serial_number;
	info->size = node->size;
	info->tee_ctx_id = node->tee_ctx_id;

	return 0;
}

int _ree_shmem_get_next_info(int32_t *cookie, ree_shmem_info *info, size_t info_size) {
	InterruptsSmpReadSpinLocker locker(wsm_node_rwlock);

	if (!info || !cookie) {
		return -EINVAL;
	}
	if (info_size != sizeof(ree_shmem_info)) {
		return -EINVAL;
	}

	if (*cookie < 0) {
		*cookie = 0;
	}

	auto it = sWSMHash.GetIterator();
	int32_t stopIndex = *cookie;
	*cookie = 0;

	while (it.HasNext()) {
		auto *node = it.Next();

		if (*cookie < stopIndex) {
			++(*cookie);
			continue;
		}

		info->id = node->id;
		info->serial_number = node->serial_number;
		info->size = node->size;
		info->tee_ctx_id = node->tee_ctx_id;
		return 0;
	}

	return -EINVAL;
}

int __sys_ree_shmem_get_info(int wsm_id, void *userInfo) {
	ree_shmem_info info;

	if (!UserTypeAccessor<ree_shmem_info>::IsArgumentValid(userInfo)) {
		return -EFAULT;
	}

	int error = _ree_shmem_get_info(wsm_id, &info, sizeof(info));

	if (error < 0) {
		return error;
	}

	return UserTypeAccessor<ree_shmem_info>::PutToUser(userInfo, info);
}

int __sys_ree_shmem_get_next_info(int32_t *userCookie, void *userInfo) {
	ree_shmem_info info;
	int32_t cookie;

	if (!UserTypeAccessor<ree_shmem_info>::IsArgumentValid(userInfo)) {
		return -EFAULT;
	}
	if (BasicUserTypeAccessor<int32_t>::GetFromUser(userCookie, cookie) < 0) {
		return -EFAULT;
	}

	int error = _ree_shmem_get_next_info(&cookie, &info, sizeof(info));

	if (error < 0) {
		return error;
	}

	if (UserTypeAccessor<ree_shmem_info>::PutToUser(userInfo, info) < 0) {
		return -EFAULT;
	}

	return BasicUserTypeAccessor<int32_t>::PutToUser(userCookie, cookie);
}

int __sys_ree_shm_create_mapping(int memid,
                                 void **_userAddress,
                                 off_t offset,
                                 size_t size,
                                 uint32_t addressSpec,
                                 uint32_t protection) {
	void *address = nullptr;
	char area_name[B_OS_NAME_LENGTH];

	// Convert from 32bit if needed
	offset = off_t_from_user(offset);

	if (protection & ~(B_READ_AREA | B_WRITE_AREA | B_NODUMP_AREA)) {
		return -EINVAL;
	}

	if (BasicUserTypeAccessor<void *>::GetFromUser(_userAddress, address) < 0) {
		return -EFAULT;
	}

	auto node = wsm_lookup_node(memid);

	if (!node) {
		return -EBADF;
	}

	if (offset < 0 || size > node->size || (size_t) offset >= node->size || (size + offset) > node->size) {
		return -EINVAL;
	}

	off_t aligned_offset = rounddown2(offset, PAGE_SIZE);
	size_t aligned_size = roundup2(offset + size, PAGE_SIZE) - aligned_offset;

	if (aligned_offset < 0 || aligned_offset + aligned_size > node->size || !aligned_size) {
		return -EINVAL;
	}

	snprintf(area_name, sizeof(area_name), "ree_shmem:%d:%zx", memid, (size_t) aligned_offset);

	vaddr_t base = (vaddr_t) address;

	area_id area = vm_map_physical_memory_pfns(getpid(),
	                                           area_name,
	                                           &base,
	                                           addressSpec,
	                                           aligned_size >> PAGE_SHIFT,
	                                           protection | B_NONSECURE_AREA,
	                                           node->pfns + (aligned_offset >> PAGE_SHIFT));

	if (area < 0) {
		return area;
	}

	address = (void *) base;

	if (BasicUserTypeAccessor<void *>::PutToUser(_userAddress, address) < 0) {
		vm_delete_area(getpid(), area, true);
		return -EFAULT;
	}

	return area;
}
