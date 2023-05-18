
#ifndef VM_USER_AREA_H
#define VM_USER_AREA_H

#include <lib/AVLTree.h>
#include <mm/VMArea.h>


struct VMUserAddressSpace;


struct VMUserArea : VMArea, AVLTreeNode {
								VMUserArea(VMAddressSpace* addressSpace,
									uint32_t areaWiring, uint32_t areaProtection);

	static	VMUserArea*			Create(VMAddressSpace* addressSpace,
									const char* name, uint32_t wiring,
									uint32_t protection, uint32_t allocationFlags);
	static	VMUserArea*			CreateReserved(VMAddressSpace* addressSpace,
									uint32_t flags, uint32_t allocationFlags);
};

struct VMUserAreaTreeDefinition {
	typedef uintptr_t Key;
	typedef VMUserArea Value;

	[[nodiscard]] static AVLTreeNode* GetAVLTreeNode(Value *value) {
		return value;
	}

	[[nodiscard]] static Value* GetValue(AVLTreeNode *node) {
		return static_cast<Value*>(node);
	}

	[[nodiscard]] static int Compare(Key a, const Value *_b) {
		Key b = _b->Base();
		if (a == b)
			return 0;
		return a < b ? -1 : 1;
	}

	[[nodiscard]] static int Compare(const Value *a, const Value *b) {
		return Compare(a->Base(), b);
	}
};

using VMUserAreaTree = AVLTree<VMUserAreaTreeDefinition>;

#endif	// VM_USER_AREA_H
