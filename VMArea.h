
#ifndef _KERNEL_VM_VM_AREA_H
#define _KERNEL_VM_VM_AREA_H

#include <mm/mm.h>
#include <core/mutex.h>
#include <lib/DoublyLinkedList.h>
#include <lib/SinglyLinkedList.h>
#include <lib/OpenHashTable.h>

struct VMAddressSpace;
struct VMCache;
struct VMKernelAddressSpace;
struct VMUserAddressSpace;

struct VMAreaUnwiredWaiter
	: public DoublyLinkedListLinkImpl<VMAreaUnwiredWaiter> {
	VMArea*					area = nullptr;
	uintptr_t				base = 0;
	size_t					size = 0;
	cond_var_t				condition;
	cond_var_entry			waitEntry;
};

using VMAreaUnwiredWaiterList = DoublyLinkedList<VMAreaUnwiredWaiter>;


struct VMAreaWiredRange : SinglyLinkedListLinkImpl<VMAreaWiredRange> {
	VMArea*					area = nullptr;
	uintptr_t				base = 0;
	size_t					size = 0;
	bool					writable = false;
	bool					implicit = false;	// range created automatically
	VMAreaUnwiredWaiterList	waiters;

	constexpr VMAreaWiredRange() = default;

	VMAreaWiredRange(uintptr_t base_, size_t size_, bool writable_, bool implicit_)
		:
		
		base(base_),
		size(size_),
		writable(writable_),
		implicit(implicit_)
	{
	}

	void SetTo(uintptr_t base_, size_t size_, bool writable_, bool implicit_)
	{
		this->area = nullptr;
		this->base = base_;
		this->size = size_;
		this->writable = writable_;
		this->implicit = implicit_;
	}

	[[nodiscard]] bool IntersectsWith(uintptr_t otherBase, size_t otherSize) const
	{
		return this->base + this->size - 1 >= otherBase && otherBase + otherSize - 1 >= this->base;
	}
};

using VMAreaWiredRangeList = SinglyLinkedList<VMAreaWiredRange>;


struct VMPageWiringInfo {
	VMAreaWiredRange	range;
	vm_paddr_t			physicalAddress{0};
							// the actual physical address corresponding to
							// the virtual address passed to vm_wire_page()
							// (i.e. with in-page offset)
	struct page*		page{nullptr};

	constexpr VMPageWiringInfo() = default;

	VMPageWiringInfo(const VMPageWiringInfo&) = delete;
	VMPageWiringInfo& operator = (const VMPageWiringInfo&) = delete;
};


struct VMArea {
public:
	enum {
		// AddWaiterIfWired() flags
		IGNORE_WRITE_WIRED_RANGES	= 0x01,	// ignore existing ranges that
											// wire for writing
	};

public:
	area_id					id;
	char					name[B_OS_NAME_LENGTH];
	uint32_t				protection;
	uint32_t				protection_max = 0;
	const uint16_t			wiring;

public:
	VMCache*				cache;
	off_t					cache_offset;
	uint32_t				cache_type;
	vm_area_mappings		mappings;
	uint8_t*				page_protections;

	VMAddressSpace*			address_space;
	VMArea*					cache_next;
	VMArea*					cache_prev;
	VMArea*					hash_next;

	mode_t					mode = 0;
	uid_t					owner_uid = 0;
	gid_t					owner_gid = 0;

			[[nodiscard]] uintptr_t			Base() const	{ return fBase; }
			[[nodiscard]] size_t			Size() const	{ return fSize; }
			[[nodiscard]] uintptr_t			End() const 	{ return fBase + fSize; }
			[[nodiscard]] uintptr_t			CommitBase() const { return fCommitBase; }

			[[nodiscard]] bool				ContainsAddress(uintptr_t address) const
									{ return address >= fBase
										&& address <= fBase + (fSize - 1); }

			[[nodiscard]] bool				IsWired() const
									{ return !fWiredRanges.IsEmpty(); }
			[[nodiscard]] bool				IsWired(uintptr_t base, size_t size) const;

			void				Wire(VMAreaWiredRange* range);
			void				Unwire(VMAreaWiredRange* range);
			VMAreaWiredRange*	Unwire(uintptr_t base, size_t size, bool writable);

			bool				AddWaiterIfWired(VMAreaUnwiredWaiter* waiter);
			bool				AddWaiterIfWired(VMAreaUnwiredWaiter* waiter,
									uintptr_t base, size_t size, uint32_t flags = 0);

			void				SetCommitBase(uintptr_t base) {
				fCommitBase = base;
			}

protected:
								VMArea(VMAddressSpace* addressSpace,
									uint32_t areaWiring, uint32_t areaProtection);
								~VMArea();

			void				Init(const char*areaName);

protected:
			friend struct VMAddressSpace;
			friend struct VMKernelAddressSpace;
			friend struct VMUserAddressSpace;

protected:
			void				SetBase(uintptr_t base)	{ fBase = base; }
			void				SetSize(size_t size)	{ fSize = size; }
			void				SetEnd(uintptr_t end) 	{ fSize = end - fBase; }

protected:
			uintptr_t			fBase;
			size_t				fSize;
			uintptr_t			fCommitBase;
			VMAreaWiredRangeList fWiredRanges;
};


struct VMAreaHashDefinition {
	using KeyType = area_id;
	using ValueType = VMArea;

	[[nodiscard]] size_t HashKey(area_id key) const
	{
		return key;
	}

	[[nodiscard]] size_t Hash(const VMArea* value) const
	{
		return HashKey(value->id);
	}

	[[nodiscard]] bool Compare(area_id key, const VMArea* value) const
	{
		return value->id == key;
	}

	[[nodiscard]] VMArea*& GetLink(VMArea* value) const
	{
		return value->hash_next;
	}
};

using VMAreaHashTable = BOpenHashTable<VMAreaHashDefinition>;


struct VMAreaHash {
	static	int					Init();

	static	int					ReadLock()
									{ return rw_lock_read_lock(&sLock); }
	static	void				ReadUnlock()
									{ rw_lock_read_unlock(&sLock); }
	static	int					WriteLock()
									{ return rw_lock_write_lock(&sLock); }
	static	void				WriteUnlock()
									{ rw_lock_write_unlock(&sLock); }

	static	VMArea*				LookupLocked(area_id id)
									{ return sTable.Lookup(id); }
	static	VMArea*				Lookup(area_id id);
	static	VMArea*				Lookup(area_id id, VMAddressSpace * addressSpace);
	static	area_id				Find(const char* name);
	static	void				Insert(VMArea* area);
	static	void				Remove(VMArea* area);

	static	VMAreaHashTable::Iterator GetIterator()
									{ return sTable.GetIterator(); }

private:
	static	rw_lock_t			sLock;
	static	VMAreaHashTable		sTable;
};

enum {
	MEMORY_TYPE_SHIFT		= 20
};

#endif	// _KERNEL_VM_VM_AREA_H
