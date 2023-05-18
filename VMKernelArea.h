
#ifndef VM_KERNEL_AREA_H
#define VM_KERNEL_AREA_H

#include <core/kconfig.h>
#include <lib/AVLTree.h>
#include <mm/VMArea.h>


struct ObjectCache;
struct VMKernelAddressSpace;
struct VMKernelArea;


struct VMKernelAddressRange : AVLTreeNode {
public:
	// range types
	enum {
		RANGE_FREE,
		RANGE_RESERVED,
		RANGE_AREA
	};

public:
	DoublyLinkedListLink<VMKernelAddressRange>		listLink;
	uintptr_t										base;
	size_t											size;
	union {
		VMKernelArea* 								area;
		struct {
			uintptr_t								base;
			uint32_t								flags;
		} reserved;
		DoublyLinkedListLink<VMKernelAddressRange>	freeListLink;
	};
	int												type;

#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
	bool											ns;
#endif

public:
	VMKernelAddressRange(uintptr_t base_, size_t size_, int type_)
		:
		base(base_),
		size(size_),
		type(type_)
	{
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		ns = false;
#endif
	}

	VMKernelAddressRange(uintptr_t rangeBase, size_t rangeSize,
		const VMKernelAddressRange* other)
		:
		base(rangeBase),
		size(rangeSize),
		type(other->type)
	{
#if defined(__arm__) && defined(CONFIG_BOARD_DISABLE_LPAE)
		ns = other->ns;
#endif

		if (type == RANGE_RESERVED) {
			reserved.base = other->reserved.base;
			reserved.flags = other->reserved.flags;
		}
	}
};


struct VMKernelAddressRangeTreeDefinition {
	typedef uintptr_t					Key;
	using Value = VMKernelAddressRange;

	AVLTreeNode* GetAVLTreeNode(Value* value) const
	{
		return value;
	}

	Value* GetValue(AVLTreeNode* node) const
	{
		return static_cast<Value*>(node);
	}

	int Compare(uintptr_t a, const Value* _b) const
	{
		uintptr_t b = _b->base;
		if (a == b)
			return 0;
		return a < b ? -1 : 1;
	}

	int Compare(const Value* a, const Value* b) const
	{
		return Compare(a->base, b);
	}
};

using VMKernelAddressRangeTree = AVLTree<VMKernelAddressRangeTreeDefinition>;


struct VMKernelAddressRangeGetFreeListLink {
	using Link = DoublyLinkedListLink<VMKernelAddressRange>;

	inline Link* operator()(VMKernelAddressRange* range) const
	{
		return &range->freeListLink;
	}

	inline const Link* operator()(const VMKernelAddressRange* range) const
	{
		return &range->freeListLink;
	}
};


struct VMKernelArea : VMArea, AVLTreeNode {
								VMKernelArea(VMAddressSpace* addressSpace,
									uint32_t areaWiring, uint32_t areaProtection);

	static	VMKernelArea*		Create(VMAddressSpace* addressSpace,
									const char* name, uint32_t wiring,
									uint32_t protection, ObjectCache* objectCache,
									uint32_t allocationFlags);

			[[nodiscard]] VMKernelAddressRange* Range() const
									{ return fRange; }
			void				SetRange(VMKernelAddressRange* range)
									{ fRange = range; }

private:
			VMKernelAddressRange* fRange = nullptr;
};


#endif	// VM_KERNEL_AREA_H
