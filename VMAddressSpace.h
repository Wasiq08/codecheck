
#ifndef _KERNEL_VM_VM_ADDRESS_SPACE_H
#define _KERNEL_VM_VM_ADDRESS_SPACE_H


#include <mm/VMArea.h>
#include <mm/mm_mapper.h>

struct virtual_address_restrictions;
struct ObjectCache;

struct VMAddressSpace {
public:
			class AreaIterator;
			class AreaRangeIterator;

public:
								VMAddressSpace(pid_t id, uintptr_t base,
									size_t size, const char* name);
	virtual						~VMAddressSpace();

	static	int					Init();

			[[nodiscard]] pid_t				ID() const				{ return fID; }
			[[nodiscard]] uintptr_t			Base() const			{ return fBase; }
			[[nodiscard]] uintptr_t			EndAddress() const		{ return fEndAddress; }
			[[nodiscard]] size_t				Size() const { return fEndAddress - fBase + 1; }
			[[nodiscard]] size_t				FreeSpace() const		{ return fFreeSpace; }
			[[nodiscard]] bool				IsBeingDeleted() const	{ return fDeleting; }

#if defined(CONFIG_64BIT) && defined(BOARD_COMPAT_LIBRARIES)
			void				ResetCompat(bool compat32);
			bool				Is32Bit() const noexcept { return fEndAddress < 0x100000000UL; }
#endif

			VMTranslationMap*	TranslationMap() const noexcept	{ return fTranslationMap; }

			int					ReadLock()
									{ return rw_lock_read_lock(&fLock); }
			void				ReadUnlock()
									{ rw_lock_read_unlock(&fLock); }
			int					WriteLock()
									{ return rw_lock_write_lock(&fLock); }
			void				WriteUnlock()
									{ rw_lock_write_unlock(&fLock); }
			bool				TryReadLock()
									{ return rw_lock_read_trylock(&fLock) == 0; }

			int32_t				RefCount()
									{ return __atomic_load_n(&fRefCount, __ATOMIC_RELAXED); }
			int32_t				FaultCount()
									{ return __atomic_load_n(&fFaultCount, __ATOMIC_RELAXED); }

	inline	void				Get()	{ __atomic_fetch_add(&fRefCount, 1, __ATOMIC_RELAXED); }
	inline	void 				Put();
			void				RemoveAndPut();

			void				IncrementFaultCount()
									{ __atomic_fetch_add(&fFaultCount, 1, __ATOMIC_RELAXED); }
			void				IncrementChangeCount()
									{ fChangeCount++; }

	[[nodiscard]] inline	bool				IsRandomizingEnabled() const
									{ return fRandomizingEnabled; }
	inline	void				SetRandomizingEnabled(bool enabled)
									{ fRandomizingEnabled = enabled; }

	inline	AreaIterator		GetAreaIterator();
	inline	AreaRangeIterator	GetAreaRangeIterator(uintptr_t address,
									size_t size);

			VMAddressSpace*&	HashTableLink()	{ return fHashTableLink; }

	virtual	int					InitObject();

	[[nodiscard]] virtual	VMArea*				FirstArea() const = 0;
	virtual	VMArea*				NextArea(VMArea* area) const = 0;

	[[nodiscard]] virtual	VMArea*				LookupArea(uintptr_t address) const = 0;
	[[nodiscard]] virtual	VMArea*				FindClosestArea(uintptr_t address, bool less) const	= 0;
	virtual	VMArea*				CreateArea(const char* name, uint32_t wiring,
									uint32_t protection,
									uint32_t allocationFlags) = 0;
	virtual	void				DeleteArea(VMArea* area,
									uint32_t allocationFlags) = 0;
	virtual	int					InsertArea(VMArea* area, size_t size,
									const virtual_address_restrictions*
										addressRestrictions,
									uint32_t allocationFlags, void** _address)
										= 0;
	virtual	void				RemoveArea(VMArea* area,
									uint32_t allocationFlags) = 0;

	virtual	bool				CanResizeArea(VMArea* area, size_t newSize) = 0;
	virtual	int					ResizeArea(VMArea* area, size_t newSize,
									uint32_t allocationFlags) = 0;
	virtual	int					ShrinkAreaHead(VMArea* area, size_t newSize,
									uint32_t allocationFlags) = 0;
	virtual	int					ShrinkAreaTail(VMArea* area, size_t newSize,
									uint32_t allocationFlags) = 0;

	virtual	int					ReserveAddressRange(size_t size,
									const virtual_address_restrictions*
										addressRestrictions,
									uint32_t flags, uint32_t allocationFlags,
									void** _address) = 0;
	virtual	int					UnreserveAddressRange(uintptr_t address,
									size_t size, uint32_t allocationFlags) = 0;
	virtual	void				UnreserveAllAddressRanges(
									uint32_t allocationFlags) = 0;

    static	int					Create(pid_t teamID, uintptr_t base, size_t size,
									bool kernel,
									VMAddressSpace** _addressSpace);

	static	pid_t				KernelID()
									{ return sKernelAddressSpace->ID(); }
	static	VMAddressSpace*		Kernel()
									{ return sKernelAddressSpace; }
	static	VMAddressSpace*		GetKernel();

	static	pid_t				CurrentID();
	static	VMAddressSpace*		GetCurrent();

	static	VMAddressSpace*		Get(pid_t teamID);

	static	VMAddressSpace*		DebugFirst();
	static	VMAddressSpace*		DebugNext(VMAddressSpace* addressSpace);
	static	VMAddressSpace*		DebugGet(pid_t teamID);

	static  ObjectCache *		GetUserAreaCache() { return sUserAreaCache; }

	[[nodiscard]] bool						IsExternalMemoryAllowed() const
									{ return fExternalMemoryAllowed; }
	void						SetExternalMemoryAllowed(bool allowed)
									{ fExternalMemoryAllowed = allowed; }

	virtual	void				Dump();

protected:
			[[nodiscard]] bool				_IsRandomized(uint32_t addressSpec) const;
			uintptr_t			_RandomizeAddress(uintptr_t start, uintptr_t end,
									size_t alignment, bool initial = false) const noexcept;

protected:
	static	void				_DeleteIfUnreferenced(pid_t id);

	static	int					_DumpCommand(int argc, char** argv);
	static	int					_DumpListCommand(int argc, char** argv);

protected:
	static constexpr size_t max_randomize_32() noexcept { return 0x800000ul; }
	static constexpr size_t max_initial_randomize_32() noexcept { return 0x2000000ul; }
#if defined(CONFIG_64BIT)
	static constexpr size_t max_randomize_64() noexcept { return 0x8000000000ul; }
	static constexpr size_t max_initial_randomize_64() noexcept { return 0x20000000000ul; }
#endif

#if defined(CONFIG_64BIT)
#if defined(BOARD_COMPAT_LIBRARIES)
	size_t max_randomize() const noexcept { return Is32Bit() ? max_randomize_32() : max_randomize_64(); }
	size_t max_initial_randomize() const noexcept { return Is32Bit() ? max_initial_randomize_32() : max_initial_randomize_64(); }
#else
	static constexpr size_t max_randomize() noexcept { return max_randomize_64(); }
	static constexpr size_t max_initial_randomize() noexcept { return max_initial_randomize_64(); }
#endif
#else
	static constexpr size_t max_randomize() noexcept { return max_randomize_32(); }
	static constexpr size_t max_initial_randomize() noexcept { return max_initial_randomize_32(); }
#endif

protected:
			struct HashDefinition;

protected:
			VMAddressSpace*		fHashTableLink;
			uintptr_t			fBase;
			uintptr_t			fEndAddress;		// base + (size - 1)
			size_t				fFreeSpace;
			rw_lock_t			fLock;
			pid_t				fID;
			int					fRefCount;
			int					fFaultCount;
			int					fChangeCount;
			VMTranslationMap*	fTranslationMap;
			bool				fRandomizingEnabled;
			bool				fDeleting;
			bool				fExternalMemoryAllowed;
	static	VMAddressSpace*		sKernelAddressSpace;
	static  ObjectCache *		sUserAreaCache;
};


void
VMAddressSpace::Put()
{
	pid_t id = fID;
	if (__atomic_fetch_sub(&fRefCount, 1, __ATOMIC_RELEASE) == 1)
		_DeleteIfUnreferenced(id);
}


class VMAddressSpace::AreaIterator {
public:
	AreaIterator() = default;

	AreaIterator(VMAddressSpace* addressSpace)
		:
		fAddressSpace(addressSpace),
		fNext(addressSpace->FirstArea())
	{
	}

	[[nodiscard]] bool HasNext() const
	{
		return fNext != nullptr;
	}

	VMArea* Next()
	{
		VMArea* result = fNext;
		if (fNext != nullptr)
			fNext = fAddressSpace->NextArea(fNext);
		return result;
	}

	void Rewind()
	{
		fNext = fAddressSpace->FirstArea();
	}

private:
	VMAddressSpace*	fAddressSpace = nullptr;
	VMArea*			fNext = nullptr;
};


class VMAddressSpace::AreaRangeIterator : public VMAddressSpace::AreaIterator {
public:
	AreaRangeIterator() = default;

	AreaRangeIterator(VMAddressSpace* addressSpace, uintptr_t address, size_t size)
		:
		fAddressSpace(addressSpace),
		fNext(nullptr),
		fAddress(address),
		fEndAddress(address + size - 1)
	{
		Rewind();
	}

	bool HasNext() const
	{
		return fNext != nullptr;
	}

	VMArea* Next()
	{
		VMArea* result = fNext;
		if (fNext != nullptr) {
			fNext = fAddressSpace->NextArea(fNext);
			if (fNext != nullptr && fNext->Base() > fEndAddress)
				fNext = nullptr;
		}

		return result;
	}

	void Rewind()
	{
		fNext = fAddressSpace->FindClosestArea(fAddress, true);
		if (fNext != nullptr && !fNext->ContainsAddress(fAddress))
			Next();
	}

private:
	VMAddressSpace*	fAddressSpace = nullptr;
	VMArea*			fNext = nullptr;
	uintptr_t		fAddress = 0;
	uintptr_t		fEndAddress = 0;
};


inline VMAddressSpace::AreaIterator
VMAddressSpace::GetAreaIterator()
{
	return AreaIterator(this);
}

inline VMAddressSpace::AreaRangeIterator
VMAddressSpace::GetAreaRangeIterator(uintptr_t address, size_t size)
{
	return AreaRangeIterator(this, address, size);
}

#ifdef __cplusplus
extern "C" {
#endif

void vm_delete_areas(struct VMAddressSpace *aspace, bool deletingAddressSpace);

#ifdef __cplusplus
}
#endif

#endif	/* _KERNEL_VM_VM_ADDRESS_SPACE_H */
