// Copyright Epic Games, Inc. All Rights Reserved.

#include "UObject/UnrealNames.h"
#include "UObject/NameBatchSerialization.h"
#include "Misc/AssertionMacros.h"
#include "Misc/MessageDialog.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "HAL/UnrealMemory.h"
#include "Templates/UnrealTemplate.h"
#include "Misc/CString.h"
#include "Misc/Crc.h"
#include "Misc/StringBuilder.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"
#include "Logging/LogMacros.h"
#include "Misc/ByteSwap.h"
#include "UObject/ObjectVersion.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ScopeRWLock.h"
#include "Containers/Set.h"
#include "Internationalization/Text.h"
#include "Internationalization/Internationalization.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/IConsoleManager.h"
#include "HAL/LowLevelMemTracker.h"
#include "Serialization/MemoryImage.h"
#include "Hash/CityHash.h"
#include "Templates/AlignmentTemplates.h"

PRAGMA_DISABLE_UNSAFE_TYPECAST_WARNINGS

// Page protection to catch FNameEntry stomps
#ifndef FNAME_WRITE_PROTECT_PAGES
#define FNAME_WRITE_PROTECT_PAGES 0
#endif
#if FNAME_WRITE_PROTECT_PAGES
#	define FNAME_BLOCK_ALIGNMENT FPlatformMemory::GetConstants().PageSize
#else
#	define FNAME_BLOCK_ALIGNMENT alignof(FNameEntry)
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUnrealNames, Log, All);

const TCHAR* LexToString(EName Ename)
{
	switch (Ename)
	{
#define REGISTER_NAME(num,namestr) case num: return TEXT(#namestr);
#include "UObject/UnrealNames.inl"
#undef REGISTER_NAME
		default:
			return TEXT("*INVALID*");
	}
}

int32 FNameEntry::GetDataOffset()
{
	return STRUCT_OFFSET(FNameEntry, AnsiName);
}

/*-----------------------------------------------------------------------------
	FName helpers. 
-----------------------------------------------------------------------------*/

static bool operator==(FNameEntryHeader A, FNameEntryHeader B)
{
	static_assert(sizeof(FNameEntryHeader) == 2, "");
	return (uint16&)A == (uint16&)B;
}

template<typename FromCharType, typename ToCharType>
ToCharType* ConvertInPlace(FromCharType* Str, uint32 Len)
{
	static_assert(TIsSame<FromCharType, ToCharType>::Value, "Unsupported conversion");
	return Str;
}

template<>
WIDECHAR* ConvertInPlace<ANSICHAR, WIDECHAR>(ANSICHAR* Str, uint32 Len)
{
	for (uint32 Index = Len; Index--; )
	{
		reinterpret_cast<WIDECHAR*>(Str)[Index] = Str[Index];
	}

	return reinterpret_cast<WIDECHAR*>(Str);
}

template<>
ANSICHAR* ConvertInPlace<WIDECHAR, ANSICHAR>(WIDECHAR* Str, uint32 Len)
{
	for (uint32 Index = 0; Index < Len; ++Index)
	{
		reinterpret_cast<ANSICHAR*>(Str)[Index] = Str[Index];
	}

	return reinterpret_cast<ANSICHAR*>(Str);
}

union FNameBuffer
{
	ANSICHAR AnsiName[NAME_SIZE];
	WIDECHAR WideName[NAME_SIZE];
};

struct FNameStringView
{
	FNameStringView() : Data(nullptr), Len(0), bIsWide(false) {}
	FNameStringView(const ANSICHAR* Str, uint32 Len_) : Ansi(Str), Len(Len_), bIsWide(false) {}
	FNameStringView(const WIDECHAR* Str, uint32 Len_) : Wide(Str), Len(Len_), bIsWide(true) {}

	union
	{
		const void* Data;
		const ANSICHAR* Ansi;
		const WIDECHAR* Wide;
	};

	uint32 Len;
	bool bIsWide;

	bool IsAnsi() const { return !bIsWide; }

	int32 BytesWithTerminator() const
	{
		return (Len + 1) * (bIsWide ? sizeof(WIDECHAR) : sizeof(ANSICHAR));
	}

	int32 BytesWithoutTerminator() const
	{
		return Len * (bIsWide ? sizeof(WIDECHAR) : sizeof(ANSICHAR));
	}
};

template<ENameCase Sensitivity>
FORCEINLINE bool EqualsSameDimensions(FNameStringView A, FNameStringView B)
{
	checkSlow(A.Len == B.Len && A.IsAnsi() == B.IsAnsi());

	int32 Len = A.Len;

	if (Sensitivity == ENameCase::CaseSensitive)
	{
		return B.IsAnsi() ? !FPlatformString::Strncmp(A.Ansi, B.Ansi, Len) : !FPlatformString::Strncmp(A.Wide, B.Wide, Len);
	}
	else
	{
		return B.IsAnsi() ? !FPlatformString::Strnicmp(A.Ansi, B.Ansi, Len) : !FPlatformString::Strnicmp(A.Wide, B.Wide, Len);
	}

}

template<ENameCase Sensitivity>
FORCEINLINE bool Equals(FNameStringView A, FNameStringView B)
{
	return (A.Len == B.Len & A.IsAnsi() == B.IsAnsi()) && EqualsSameDimensions<Sensitivity>(A, B);
}

// Minimize stack lifetime of large decode buffers
#ifdef WITH_CUSTOM_NAME_ENCODING
#define OUTLINE_DECODE_BUFFER FORCENOINLINE
#else
#define OUTLINE_DECODE_BUFFER
#endif

template<ENameCase Sensitivity>
OUTLINE_DECODE_BUFFER bool EqualsSameDimensions(const FNameEntry& Entry, FNameStringView Name)
{
	FNameBuffer DecodeBuffer;
	return EqualsSameDimensions<Sensitivity>(Entry.MakeView(DecodeBuffer), Name);
}

/** Remember to update natvis if you change these */
enum { FNameMaxBlockBits = 13 }; // Limit block array a bit, still allowing 8k * block size = 1GB - 2G of FName entry data
enum { FNameBlockOffsetBits = 16 };
enum { FNameMaxBlocks = 1 << FNameMaxBlockBits };
enum { FNameBlockOffsets = 1 << FNameBlockOffsetBits };

/** An unpacked FNameEntryId */
struct FNameEntryHandle
{
	uint32 Block = 0;
	uint32 Offset = 0;

	FNameEntryHandle(uint32 InBlock, uint32 InOffset)
		: Block(InBlock)
		, Offset(InOffset)
	{}

	FNameEntryHandle(FNameEntryId Id)
		: Block(Id.ToUnstableInt() >> FNameBlockOffsetBits)
		, Offset(Id.ToUnstableInt() & (FNameBlockOffsets - 1))
	{}

	operator FNameEntryId() const
	{
		return FNameEntryId::FromUnstableInt(Block << FNameBlockOffsetBits | Offset);
	}

	explicit operator bool() const { return Block | Offset; }
};

static uint32 GetTypeHash(FNameEntryHandle Handle)
{
	return (Handle.Block << (32 - FNameMaxBlockBits)) + Handle.Block // Let block index impact most hash bits
		+ (Handle.Offset << FNameBlockOffsetBits) + Handle.Offset // Let offset impact most hash bits
		+ (Handle.Offset >> 4); // Reduce impact of non-uniformly distributed entry name lengths 
}

uint32 GetTypeHash(FNameEntryId Id)
{
	return GetTypeHash(FNameEntryHandle(Id));
}

FArchive& operator<<(FArchive& Ar, FNameEntryId& Id)
{
	if (Ar.IsLoading())
	{
		uint32 UnstableInt = 0;
		Ar << UnstableInt;
		Id = FNameEntryId::FromUnstableInt(UnstableInt);
	}
	else
	{
		uint32 UnstableInt = Id.ToUnstableInt();
		Ar << UnstableInt;
	}

	return Ar;
}

FNameEntryId FNameEntryId::FromUnstableInt(uint32 Value)
{
	FNameEntryId Id;
	Id.Value = Value;
	return Id;
}

struct FNameSlot
{
	// Use the remaining few bits to store a hash that can determine inequality
	// during probing without touching entry data
	static constexpr uint32 EntryIdBits = FNameMaxBlockBits + FNameBlockOffsetBits;
	static constexpr uint32 EntryIdMask = (1 << EntryIdBits) - 1;
	static constexpr uint32 ProbeHashShift = EntryIdBits;
	static constexpr uint32 ProbeHashMask = ~EntryIdMask;
	
	FNameSlot() {}
	FNameSlot(FNameEntryId Value, uint32 ProbeHash)
		: IdAndHash(Value.ToUnstableInt() | ProbeHash)
	{
		check(!(Value.ToUnstableInt() & ProbeHashMask) && !(ProbeHash & EntryIdMask) && Used());
	}

	FNameEntryId GetId() const { return FNameEntryId::FromUnstableInt(IdAndHash & EntryIdMask); }
	uint32 GetProbeHash() const { return IdAndHash & ProbeHashMask; }
	
	bool operator==(FNameSlot Rhs) const { return IdAndHash == Rhs.IdAndHash; }

	bool Used() const { return !!IdAndHash;  }
private:
	uint32 IdAndHash = 0;
};

/**
 * Thread-safe paged FNameEntry allocator
 */
class FNameEntryAllocator
{
public:
	enum { Stride = alignof(FNameEntry) };
	enum { BlockSizeBytes = Stride * FNameBlockOffsets };

	/** Initializes all member variables. */
	FNameEntryAllocator()
	{
		LLM_SCOPE(ELLMTag::FName);
		Blocks[0] = (uint8*)FMemory::MallocPersistentAuxiliary(BlockSizeBytes, FNAME_BLOCK_ALIGNMENT);
	}

	~FNameEntryAllocator()
	{
		for (uint32 Index = 0; Index <= CurrentBlock; ++Index)
		{
			FMemory::Free(Blocks[Index]);
		}
	}

	void ReserveBlocks(uint32 Num)
	{
		FWriteScopeLock _(Lock);

		for (uint32 Idx = Num - 1; Idx > CurrentBlock && Blocks[Idx] == nullptr; --Idx)
		{
			Blocks[Idx] = AllocBlock();
		}
	}


	/**
	 * Allocates the requested amount of bytes and returns an id that can be used to access them
	 *
	 * @param   Size  Size in bytes to allocate, 
	 * @return  Allocation of passed in size cast to a FNameEntry pointer.
	 */
	template <class ScopeLock>
	FNameEntryHandle Allocate(uint32 Bytes)
	{
		Bytes = Align(Bytes, alignof(FNameEntry));
		check(Bytes <= BlockSizeBytes);

		ScopeLock _(Lock);

		// Allocate a new pool if current one is exhausted. We don't worry about a little bit
		// of waste at the end given the relative size of pool to average and max allocation.
		if (BlockSizeBytes - CurrentByteCursor < Bytes)
		{
			AllocateNewBlock();
		}

		// Use current cursor position for this allocation and increment cursor for next allocation
		uint32 ByteOffset = CurrentByteCursor;
		CurrentByteCursor += Bytes;
		
		check(ByteOffset % Stride == 0 && ByteOffset / Stride < FNameBlockOffsets);

		return FNameEntryHandle(CurrentBlock, ByteOffset / Stride);
	}

	template<class ScopeLock>
	FNameEntryHandle Create(FNameStringView Name, TOptional<FNameEntryId> ComparisonId, FNameEntryHeader Header)
	{
		FNameEntryHandle Handle = Allocate<ScopeLock>(FNameEntry::GetDataOffset() + Name.BytesWithoutTerminator());
		FNameEntry& Entry = Resolve(Handle);

#if WITH_CASE_PRESERVING_NAME
		Entry.ComparisonId = ComparisonId.IsSet() ? ComparisonId.GetValue() : FNameEntryId(Handle);
#endif

		Entry.Header = Header;
		
		if (Name.bIsWide)
		{
			Entry.StoreName(Name.Wide, Name.Len);
		}
		else
		{
			Entry.StoreName(Name.Ansi, Name.Len);
		}

		return Handle;
	}

	FNameEntry& Resolve(FNameEntryHandle Handle) const
	{
		// Lock not needed
		return *reinterpret_cast<FNameEntry*>(Blocks[Handle.Block] + Stride * Handle.Offset);
	}

	void BatchLock() const
	{
		Lock.WriteLock();
	}

	void BatchUnlock() const
	{
		Lock.WriteUnlock();
	}

	/** Returns the number of blocks that have been allocated so far for names. */
	uint32 NumBlocks() const
	{
		return CurrentBlock + 1;
	}
	
	uint8** GetBlocksForDebugVisualizer() { return Blocks; }

	void DebugDump(TArray<const FNameEntry*>& Out) const
	{
		FRWScopeLock _(Lock, FRWScopeLockType::SLT_ReadOnly);

		for (uint32 BlockIdx = 0; BlockIdx < CurrentBlock; ++BlockIdx)
		{
			DebugDumpBlock(Blocks[BlockIdx], BlockSizeBytes, Out);
		}

		DebugDumpBlock(Blocks[CurrentBlock], CurrentByteCursor, Out);
	}

private:
	static void DebugDumpBlock(const uint8* It, uint32 BlockSize, TArray<const FNameEntry*>& Out)
	{
		const uint8* End = It + BlockSize - FNameEntry::GetDataOffset();
		while (It < End)
		{
			const FNameEntry* Entry = (const FNameEntry*)It;
			if (uint32 Len = Entry->Header.Len)
			{
				Out.Add(Entry);
				It += FNameEntry::GetSize(Len, !Entry->IsWide());
			}
			else // Null-terminator entry found
			{
				break;
			}
		}
	}

	static uint8* AllocBlock()
	{
		return (uint8*)FMemory::MallocPersistentAuxiliary(BlockSizeBytes, FNAME_BLOCK_ALIGNMENT);
	}
	
	void AllocateNewBlock()
	{
		LLM_SCOPE(ELLMTag::FName);
		// Null-terminate final entry to allow DebugDump() entry iteration
		if (CurrentByteCursor + FNameEntry::GetDataOffset() <= BlockSizeBytes)
		{
			FNameEntry* Terminator = (FNameEntry*)(Blocks[CurrentBlock] + CurrentByteCursor);
			Terminator->Header.Len = 0;
		}

#if FNAME_WRITE_PROTECT_PAGES
		FPlatformMemory::PageProtect(Blocks[CurrentBlock], BlockSizeBytes, /* read */ true, /* write */ false);
#endif
		++CurrentBlock;
		CurrentByteCursor = 0;

		check(CurrentBlock < FNameMaxBlocks);

		// Allocate block unless it's already reserved
		if (Blocks[CurrentBlock] == nullptr)
		{
			Blocks[CurrentBlock] = AllocBlock();
		}
	}

	mutable FRWLock Lock;
	uint32 CurrentBlock = 0;
	uint32 CurrentByteCursor = 0;
	uint8* Blocks[FNameMaxBlocks] = {};
};

// Increasing shards reduces contention but uses more memory and adds cache pressure.
// Reducing contention matters when multiple threads create FNames in parallel.
// Contention exists in some tool scenarios, for instance between main thread
// and asset data gatherer thread during editor startup.
#if WITH_CASE_PRESERVING_NAME
enum { FNamePoolShardBits = 10 };
#else
enum { FNamePoolShardBits = 4 };
#endif

enum { FNamePoolShards = 1 << FNamePoolShardBits };
enum { FNamePoolInitialSlotBits = 8 };
enum { FNamePoolInitialSlotsPerShard = 1 << FNamePoolInitialSlotBits };

/** Hashes name into 64 bits that determines shard and slot index.
 *	
 *	A small part of the hash is also stored in unused bits of the slot and entry. 
 *	The former optimizes linear probing by accessing less entry data.
 *	The latter optimizes linear probing by avoiding copying and deobfuscating entry data.
 *
 *	The slot index could be stored in the slot, at least in non shipping / test configs.
 *	This costs memory by doubling slot size but would essentially never touch entry data
 *	nor copy and deobfuscate a name needlessy. It also allows growing the hash table
 *	without rehashing the strings, since the unmasked slot index would be known.
 */
struct FNameHash
{
	uint32 ShardIndex;
	uint32 UnmaskedSlotIndex; // Determines at what slot index to start probing
	uint32 SlotProbeHash; // Helps cull equality checks (decode + strnicmp) when probing slots
	FNameEntryHeader EntryProbeHeader; // Helps cull equality checks when probing inspects entries

	static constexpr uint64 AlgorithmId = 0xC1640000;

	template<class CharType>
	static uint64 GenerateHash(const CharType* Str, int32 Len)
	{
		return CityHash64(reinterpret_cast<const char*>(Str), Len * sizeof(CharType));
	}

	template<class CharType>
	static uint64 GenerateLowerCaseHash(const CharType* Str, uint32 Len);

	template<class CharType>
	FNameHash(const CharType* Str, int32 Len)
		: FNameHash(Str, Len, GenerateHash(Str, Len))
	{}

	template<class CharType>
	FNameHash(const CharType* Str, int32 Len, uint64 Hash)
	{
		uint32 Hi = static_cast<uint32>(Hash >> 32);
		uint32 Lo = static_cast<uint32>(Hash);

		// "None" has FNameEntryId with a value of zero
		// Always set a bit in SlotProbeHash for "None" to distinguish unused slot values from None
		// @see FNameSlot::Used()
		uint32 IsNoneBit = IsAnsiNone(Str, Len) << FNameSlot::ProbeHashShift;

		static constexpr uint32 ShardMask = FNamePoolShards - 1;
		static_assert((ShardMask & FNameSlot::ProbeHashMask) == 0, "Masks overlap");

		ShardIndex = Hi & ShardMask;
		UnmaskedSlotIndex = Lo;
		SlotProbeHash = (Hi & FNameSlot::ProbeHashMask) | IsNoneBit;
		EntryProbeHeader.Len = Len;
		EntryProbeHeader.bIsWide = sizeof(CharType) == sizeof(WIDECHAR);

		// When we always use lowercase hashing, we can store parts of the hash in the entry
		// to avoid copying and decoding entries needlessly. WITH_CUSTOM_NAME_ENCODING
		// that makes this important is normally on when WITH_CASE_PRESERVING_NAME is off.
#if !WITH_CASE_PRESERVING_NAME		
		static constexpr uint32 EntryProbeMask = (1u << FNameEntryHeader::ProbeHashBits) - 1; 
		EntryProbeHeader.LowercaseProbeHash = static_cast<uint16>((Hi >> FNamePoolShardBits) & EntryProbeMask);
#endif
	}
	
	uint32 GetProbeStart(uint32 SlotMask) const
	{
		return UnmaskedSlotIndex & SlotMask;
	}

	static uint32 GetProbeStart(uint32 UnmaskedSlotIndex, uint32 SlotMask)
	{
		return UnmaskedSlotIndex & SlotMask;
	}

	static uint32 IsAnsiNone(const WIDECHAR* Str, int32 Len)
	{
		return 0;
	}

	static uint32 IsAnsiNone(const ANSICHAR* Str, int32 Len)
	{
		if (Len != 4)
		{
			return 0;
		}

#if PLATFORM_LITTLE_ENDIAN
		static constexpr uint32 NoneAsInt = 0x454e4f4e;
#else
		static constexpr uint32 NoneAsInt = 0x4e4f4e45;
#endif
		static constexpr uint32 ToUpperMask = 0xdfdfdfdf;

		uint32 FourChars = FPlatformMemory::ReadUnaligned<uint32>(Str);
		return (FourChars & ToUpperMask) == NoneAsInt;
	}

	bool operator==(const FNameHash& Rhs) const
	{
		return  ShardIndex == Rhs.ShardIndex &&
				UnmaskedSlotIndex == Rhs.UnmaskedSlotIndex &&
				SlotProbeHash == Rhs.SlotProbeHash &&
				EntryProbeHeader == Rhs.EntryProbeHeader;
	}
};

template<class CharType>
FORCENOINLINE uint64 FNameHash::GenerateLowerCaseHash(const CharType* Str, uint32 Len)
{
	CharType LowerStr[NAME_SIZE];
	for (uint32 I = 0; I < Len; ++I)
	{
		LowerStr[I] = TChar<CharType>::ToLower(Str[I]);
	}

	return FNameHash::GenerateHash(LowerStr, Len);
}

template<class CharType>
FORCENOINLINE FNameHash HashLowerCase(const CharType* Str, uint32 Len)
{
	CharType LowerStr[NAME_SIZE];
	for (uint32 I = 0; I < Len; ++I)
	{
		LowerStr[I] = TChar<CharType>::ToLower(Str[I]);
	}
	return FNameHash(LowerStr, Len);
}

template<ENameCase Sensitivity>
FNameHash HashName(FNameStringView Name);

template<>
FNameHash HashName<ENameCase::IgnoreCase>(FNameStringView Name)
{
	return Name.IsAnsi() ? HashLowerCase(Name.Ansi, Name.Len) : HashLowerCase(Name.Wide, Name.Len);
}
template<>
FNameHash HashName<ENameCase::CaseSensitive>(FNameStringView Name)
{
	return Name.IsAnsi() ? FNameHash(Name.Ansi, Name.Len) : FNameHash(Name.Wide, Name.Len);
}

template<ENameCase Sensitivity>
struct FNameValue
{
	explicit FNameValue(FNameStringView InName)
		: Name(InName)
		, Hash(HashName<Sensitivity>(InName))
	{}

	FNameValue(FNameStringView InName, FNameHash InHash)
		: Name(InName)
		, Hash(InHash)
	{}

	FNameStringView Name;
	FNameHash Hash;
	TOptional<FNameEntryId> ComparisonId;
};

using FNameComparisonValue = FNameValue<ENameCase::IgnoreCase>;
#if WITH_CASE_PRESERVING_NAME
using FNameDisplayValue = FNameValue<ENameCase::CaseSensitive>;
#endif

// For prelocked batch insertions
struct FNullScopeLock
{
	FNullScopeLock(FRWLock&) {}
};

class alignas(PLATFORM_CACHE_LINE_SIZE) FNamePoolShardBase : FNoncopyable
{
public:
	void Initialize(FNameEntryAllocator& InEntries)
	{
		LLM_SCOPE(ELLMTag::FName);
		Entries = &InEntries;

		Slots = (FNameSlot*)FMemory::Malloc(FNamePoolInitialSlotsPerShard * sizeof(FNameSlot), alignof(FNameSlot));
		memset(Slots, 0, FNamePoolInitialSlotsPerShard * sizeof(FNameSlot));
		CapacityMask = FNamePoolInitialSlotsPerShard - 1;
	}

	// This and ~FNamePool() is not called during normal shutdown
	// but only via explicit FName::TearDown() call
	~FNamePoolShardBase()
	{
		FMemory::Free(Slots);
		UsedSlots = 0;
		CapacityMask = 0;
		Slots = nullptr;
		NumCreatedEntries = 0;
		NumCreatedWideEntries = 0;
	}

	uint32 Capacity() const	{ return CapacityMask + 1; }

	uint32 NumCreated() const { return NumCreatedEntries; }
	uint32 NumCreatedWide() const { return NumCreatedWideEntries; }

	// Used for batch insertion together with Insert<FNullScopeLock>()
	void BatchLock() const	 { Lock.WriteLock(); }
	void BatchUnlock() const { Lock.WriteUnlock(); }

protected:
	enum { LoadFactorQuotient = 9, LoadFactorDivisor = 10 }; // I.e. realloc slots when 90% full

	mutable FRWLock Lock;
	uint32 UsedSlots = 0;
	uint32 CapacityMask = 0;
	FNameSlot* Slots = nullptr;
	FNameEntryAllocator* Entries = nullptr;
	uint32 NumCreatedEntries = 0;
	uint32 NumCreatedWideEntries = 0;


	template<ENameCase Sensitivity>
	FORCEINLINE static bool EntryEqualsValue(const FNameEntry& Entry, const FNameValue<Sensitivity>& Value)
	{
		return Entry.Header == Value.Hash.EntryProbeHeader && EqualsSameDimensions<Sensitivity>(Entry, Value.Name);
	}
};

template<ENameCase Sensitivity>
class FNamePoolShard : public FNamePoolShardBase
{
public:
	FNameEntryId Find(const FNameValue<Sensitivity>& Value) const
	{
		FRWScopeLock _(Lock, FRWScopeLockType::SLT_ReadOnly);

		return Probe(Value).GetId();
	}

	template<class ScopeLock = FWriteScopeLock>
	FORCEINLINE FNameEntryId Insert(const FNameValue<Sensitivity>& Value, bool& bCreatedNewEntry)
	{
		ScopeLock _(Lock);
		FNameSlot& Slot = Probe(Value);

		if (Slot.Used())
		{
			return Slot.GetId();
		}

		FNameEntryId NewEntryId = Entries->Create<ScopeLock>(Value.Name, Value.ComparisonId, Value.Hash.EntryProbeHeader);

		ClaimSlot(Slot, FNameSlot(NewEntryId, Value.Hash.SlotProbeHash));

		++NumCreatedEntries;
		NumCreatedWideEntries += Value.Name.bIsWide;
		bCreatedNewEntry = true;

		return NewEntryId;
	}

	void InsertExistingEntry(FNameHash Hash, FNameEntryId ExistingId)
	{
		FNameSlot NewLookup(ExistingId, Hash.SlotProbeHash);

		FRWScopeLock _(Lock, FRWScopeLockType::SLT_Write);
		 
		FNameSlot& Slot = Probe(Hash.UnmaskedSlotIndex, [=](FNameSlot Old) { return Old == NewLookup; });
		if (!Slot.Used())
		{
			ClaimSlot(Slot, NewLookup);
		}
	}

	void Reserve(uint32 Num)
	{
		uint32 WantedCapacity = FMath::RoundUpToPowerOfTwo(Num * LoadFactorDivisor / LoadFactorQuotient);

		FWriteScopeLock _(Lock);
		if (WantedCapacity > Capacity())
		{
			Grow(WantedCapacity);
		}
	}

private:
	void ClaimSlot(FNameSlot& UnusedSlot, FNameSlot NewValue)
	{
		UnusedSlot = NewValue;

		++UsedSlots;
		if (UsedSlots * LoadFactorDivisor >= LoadFactorQuotient * Capacity())
		{
			Grow();
		}
	}

	void Grow()
	{
		Grow(Capacity() * 2);
	}

	void Grow(const uint32 NewCapacity)
	{
		LLM_SCOPE(ELLMTag::FName);
		FNameSlot* const OldSlots = Slots;
		const uint32 OldUsedSlots = UsedSlots;
		const uint32 OldCapacity = Capacity();

		Slots = (FNameSlot*)FMemory::Malloc(NewCapacity * sizeof(FNameSlot), alignof(FNameSlot));
		memset(Slots, 0, NewCapacity * sizeof(FNameSlot));
		UsedSlots = 0;
		CapacityMask = NewCapacity - 1;


		for (uint32 OldIdx = 0; OldIdx < OldCapacity; ++OldIdx)
		{
			const FNameSlot& OldSlot = OldSlots[OldIdx];
			if (OldSlot.Used())
			{
				FNameHash Hash = Rehash(OldSlot.GetId());
				FNameSlot& NewSlot = Probe(Hash.UnmaskedSlotIndex, [](FNameSlot Slot) { return false; });
				NewSlot = OldSlot;
				++UsedSlots;
			}
		}

		check(OldUsedSlots == UsedSlots);

		FMemory::Free(OldSlots);
	}

	/** Find slot containing value or the first free slot that should be used to store it  */
	FORCEINLINE FNameSlot& Probe(const FNameValue<Sensitivity>& Value) const
	{
		return Probe(Value.Hash.UnmaskedSlotIndex, 
			[&](FNameSlot Slot)	{ return Slot.GetProbeHash() == Value.Hash.SlotProbeHash && 
									EntryEqualsValue<Sensitivity>(Entries->Resolve(Slot.GetId()), Value); });
	}

	/** Find slot that fulfills predicate or the first free slot  */
	template<class PredicateFn>
	FORCEINLINE FNameSlot& Probe(uint32 UnmaskedSlotIndex, PredicateFn Predicate) const
	{
		const uint32 Mask = CapacityMask;
		for (uint32 I = FNameHash::GetProbeStart(UnmaskedSlotIndex, Mask); true; I = (I + 1) & Mask)
		{
			FNameSlot& Slot = Slots[I];
			if (!Slot.Used() || Predicate(Slot))
			{
				return Slot;
			}
		}
	}

	OUTLINE_DECODE_BUFFER FNameHash Rehash(FNameEntryId EntryId)
	{
		const FNameEntry& Entry = Entries->Resolve(EntryId);
		FNameBuffer DecodeBuffer;
		return HashName<Sensitivity>(Entry.MakeView(DecodeBuffer));
	}
};


class FNamePool
{
public:
	FNamePool();
	
	void			Reserve(uint32 NumBlocks, uint32 NumEntries);
	FNameEntryId	Store(FNameStringView View);
	FNameEntryId	Find(FNameStringView View) const;
	FNameEntryId	Find(EName Ename) const;
	const EName*	FindEName(FNameEntryId Id) const;

	/** @pre !!Handle */
	FNameEntry&		Resolve(FNameEntryHandle Handle) const { return Entries.Resolve(Handle); }

	bool			IsValid(FNameEntryHandle Handle) const;

	void			BatchLock();
	FNameEntryId	BatchStore(const FNameComparisonValue& ComparisonValue);
	void			BatchUnlock();

	/// Stats and debug related functions ///

	uint32			NumEntries() const;
	uint32			NumAnsiEntries() const;
	uint32			NumWideEntries() const;
	uint32			NumBlocks() const { return Entries.NumBlocks(); }
	uint32			NumSlots() const;
	void			LogStats(FOutputDevice& Ar) const;
	uint8**			GetBlocksForDebugVisualizer() { return Entries.GetBlocksForDebugVisualizer(); }
	TArray<const FNameEntry*> DebugDump() const;

private:
	enum { MaxENames = 512 };

	FNameEntryAllocator Entries;

#if WITH_CASE_PRESERVING_NAME
	FNamePoolShard<ENameCase::CaseSensitive> DisplayShards[FNamePoolShards];
#endif
	FNamePoolShard<ENameCase::IgnoreCase> ComparisonShards[FNamePoolShards];

	// Put constant lookup on separate cache line to avoid it being constantly invalidated by insertion
	alignas(PLATFORM_CACHE_LINE_SIZE) FNameEntryId ENameToEntry[NAME_MaxHardcodedNameIndex] = {};
	uint32 LargestEnameUnstableId;
	TMap<FNameEntryId, EName, TInlineSetAllocator<MaxENames>> EntryToEName;
};

FNamePool::FNamePool()
{
	for (FNamePoolShardBase& Shard : ComparisonShards)
	{
		Shard.Initialize(Entries);
	}

#if WITH_CASE_PRESERVING_NAME
	for (FNamePoolShardBase& Shard : DisplayShards)
	{
		Shard.Initialize(Entries);
	}
#endif

	// Register all hardcoded names
#define REGISTER_NAME(num, name) ENameToEntry[num] = Store(FNameStringView(#name, FCStringAnsi::Strlen(#name)));
#include "UObject/UnrealNames.inl"
#undef REGISTER_NAME

	// Make reverse mapping
	LargestEnameUnstableId = 0;
	for (uint32 ENameIndex = 0; ENameIndex < NAME_MaxHardcodedNameIndex; ++ENameIndex)
	{
		if (ENameIndex == NAME_None || ENameToEntry[ENameIndex])
		{
			EntryToEName.Add(ENameToEntry[ENameIndex], (EName)ENameIndex);
			LargestEnameUnstableId = FMath::Max(LargestEnameUnstableId, ENameToEntry[ENameIndex].ToUnstableInt());
		}
	}

	// Verify all ENames are unique
	if (NumAnsiEntries() != EntryToEName.Num())
	{
		// we can't print out here because there may be no log yet if this happens before main starts
		if (FPlatformMisc::IsDebuggerPresent())
		{
			UE_DEBUG_BREAK();
		}
		else
		{
			FPlatformMisc::PromptForRemoteDebugging(false);
			FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("UnrealEd", "DuplicatedHardcodedName", "Duplicate hardcoded name"));
			FPlatformMisc::RequestExit(false);
		}
	}
}

static bool IsPureAnsi(const WIDECHAR* Str, const int32 Len)
{
	// Consider SSE version if this function takes significant amount of time
	uint32 Result = 0;
	for (int32 I = 0; I < Len; ++I)
	{
		Result |= TChar<WIDECHAR>::ToUnsigned(Str[I]);
	}
	return !(Result & 0xffffff80u);
}

FNameEntryId FNamePool::Find(EName Ename) const
{
	checkSlow(Ename < NAME_MaxHardcodedNameIndex);
	return ENameToEntry[Ename];
}

FNameEntryId FNamePool::Find(FNameStringView Name) const
{
#if WITH_CASE_PRESERVING_NAME
	FNameDisplayValue DisplayValue(Name);
	if (FNameEntryId Existing = DisplayShards[DisplayValue.Hash.ShardIndex].Find(DisplayValue))
	{
		return Existing;
	}
#endif

	FNameComparisonValue ComparisonValue(Name);
	return ComparisonShards[ComparisonValue.Hash.ShardIndex].Find(ComparisonValue);
}

FNameEntryId FNamePool::Store(FNameStringView Name)
{
#if WITH_CASE_PRESERVING_NAME
	FNameDisplayValue DisplayValue(Name);
	FNamePoolShard<ENameCase::CaseSensitive>& DisplayShard = DisplayShards[DisplayValue.Hash.ShardIndex];
	if (FNameEntryId Existing = DisplayShard.Find(DisplayValue))
	{
		return Existing;
	}
#endif

	bool bAdded = false;

	// Insert comparison name first since display value must contain comparison name
	FNameComparisonValue ComparisonValue(Name);
	FNameEntryId ComparisonId = ComparisonShards[ComparisonValue.Hash.ShardIndex].Insert(ComparisonValue, bAdded);

#if WITH_CASE_PRESERVING_NAME
	// Check if ComparisonId can be used as DisplayId
	if (bAdded || EqualsSameDimensions<ENameCase::CaseSensitive>(Resolve(ComparisonId), Name))
	{
		DisplayShard.InsertExistingEntry(DisplayValue.Hash, ComparisonId);
		return ComparisonId;
	}
	else
	{
		DisplayValue.ComparisonId = ComparisonId;
		return DisplayShard.Insert(DisplayValue, bAdded);
	}
#else
	return ComparisonId;
#endif
}

void FNamePool::BatchLock()
{
	for (const FNamePoolShardBase& Shard : ComparisonShards)
	{
		Shard.BatchLock();
	}

	// Acquire entry allocator lock after shard locks
	Entries.BatchLock();
}

FORCEINLINE FNameEntryId FNamePool::BatchStore(const FNameComparisonValue& ComparisonValue)
{
	bool bCreatedNewEntry;
	return ComparisonShards[ComparisonValue.Hash.ShardIndex].Insert<FNullScopeLock>(ComparisonValue, bCreatedNewEntry);
}

void FNamePool::BatchUnlock()
{
	Entries.BatchUnlock();

	for (int32 Idx = FNamePoolShards - 1; Idx >= 0; --Idx)
	{
		ComparisonShards[Idx].BatchUnlock();
	}
}

uint32 FNamePool::NumEntries() const
{
	uint32 Out = 0;
#if WITH_CASE_PRESERVING_NAME
	for (const FNamePoolShardBase& Shard : DisplayShards)
	{
		Out += Shard.NumCreated();
	}
#endif
	for (const FNamePoolShardBase& Shard : ComparisonShards)
	{
		Out += Shard.NumCreated();
	}

	return Out;
}

uint32 FNamePool::NumAnsiEntries() const
{
	return NumEntries() - NumWideEntries();
}

uint32 FNamePool::NumWideEntries() const
{
	uint32 Out = 0;
#if WITH_CASE_PRESERVING_NAME
	for (const FNamePoolShardBase& Shard : DisplayShards)
	{
		Out += Shard.NumCreatedWide();
	}
#endif
	for (const FNamePoolShardBase& Shard : ComparisonShards)
	{
		Out += Shard.NumCreatedWide();
	}

	return Out;
}

uint32 FNamePool::NumSlots() const
{
	uint32 SlotCapacity = 0;
#if WITH_CASE_PRESERVING_NAME
	for (const FNamePoolShardBase& Shard : DisplayShards)
	{
		SlotCapacity += Shard.Capacity();
	}
#endif
	for (const FNamePoolShardBase& Shard : ComparisonShards)
	{
		SlotCapacity += Shard.Capacity();
	}

	return SlotCapacity;
}

void FNamePool::LogStats(FOutputDevice& Ar) const
{
	Ar.Logf(TEXT("%i FNames using in %ikB + %ikB"), NumEntries(), sizeof(FNamePool), Entries.NumBlocks() * FNameEntryAllocator::BlockSizeBytes / 1024);
}

TArray<const FNameEntry*> FNamePool::DebugDump() const
{
	TArray<const FNameEntry*> Out;
	Out.Reserve(NumEntries());
	Entries.DebugDump(Out);
	return Out;
}

bool FNamePool::IsValid(FNameEntryHandle Handle) const
{
	return Handle.Block < Entries.NumBlocks();
}

const EName* FNamePool::FindEName(FNameEntryId Id) const
{
	return Id.ToUnstableInt() > LargestEnameUnstableId ? nullptr : EntryToEName.Find(Id);
}

void FNamePool::Reserve(uint32 NumBytes, uint32 InNumEntries)
{
	uint32 NumBlocks = NumBytes / FNameEntryAllocator::BlockSizeBytes + 1;
	Entries.ReserveBlocks(NumBlocks);

	if (NumEntries() < InNumEntries)
	{
		uint32 NumEntriesPerShard = InNumEntries / FNamePoolShards + 1;

	#if WITH_CASE_PRESERVING_NAME
		for (FNamePoolShard<ENameCase::CaseSensitive>& Shard : DisplayShards)
		{
			Shard.Reserve(NumEntriesPerShard);
		}
	#endif
		for (FNamePoolShard<ENameCase::IgnoreCase>& Shard : ComparisonShards)
		{
			Shard.Reserve(NumEntriesPerShard);
		}
	}
}

static bool bNamePoolInitialized;
alignas(FNamePool) static uint8 NamePoolData[sizeof(FNamePool)];

// Only call this once per public FName function called
//
// Not using magic statics to run as little code as possible
static FNamePool& GetNamePool()
{
	if (bNamePoolInitialized)
	{
		return *(FNamePool*)NamePoolData;
	}

	FNamePool* Singleton = new (NamePoolData) FNamePool;
	bNamePoolInitialized = true;
	return *Singleton;
}

// Only call from functions guaranteed to run after FName lazy initialization
static FNamePool& GetNamePoolPostInit()
{
	checkSlow(bNamePoolInitialized);
	return (FNamePool&)NamePoolData;
}

bool operator==(FNameEntryId Id, EName Ename)
{
	return Id == GetNamePoolPostInit().Find(Ename);
}

static int32 CompareDifferentIdsAlphabetically(FNameEntryId AId, FNameEntryId BId)
{
	checkSlow(AId != BId);

	FNamePool& Pool = GetNamePool();
	FNameBuffer ABuffer, BBuffer;
	FNameStringView AView =	Pool.Resolve(AId).MakeView(ABuffer);
	FNameStringView BView =	Pool.Resolve(BId).MakeView(BBuffer);

	// If only one view is wide, convert the ansi view to wide as well
	if (AView.bIsWide != BView.bIsWide)
	{
		FNameStringView& AnsiView = AView.bIsWide ? BView : AView;
		FNameBuffer& AnsiBuffer =	AView.bIsWide ? BBuffer : ABuffer;

#ifndef WITH_CUSTOM_NAME_ENCODING
		FPlatformMemory::Memcpy(AnsiBuffer.AnsiName, AnsiView.Ansi, AnsiView.Len * sizeof(ANSICHAR));
		AnsiView.Ansi = AnsiBuffer.AnsiName;
#endif

		ConvertInPlace<ANSICHAR, WIDECHAR>(AnsiBuffer.AnsiName, AnsiView.Len);
		AnsiView.bIsWide = true;
	}

	int32 MinLen = FMath::Min(AView.Len, BView.Len);
	if (int32 StrDiff = AView.bIsWide ?	FCStringWide::Strnicmp(AView.Wide, BView.Wide, MinLen) :
										FCStringAnsi::Strnicmp(AView.Ansi, BView.Ansi, MinLen))
	{
		return StrDiff;
	}

	return AView.Len - BView.Len;
}

int32 FNameEntryId::CompareLexical(FNameEntryId Rhs) const
{
	return Value != Rhs.Value && CompareDifferentIdsAlphabetically(*this, Rhs);
}

#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
	void CallNameCreationHook();
#else
	FORCEINLINE void CallNameCreationHook()
	{
	}
#endif

static FNameEntryId DebugCastNameEntryId(int32 Id) { return (FNameEntryId&)(Id); }

/**
* Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Class->Name.Index)". 
*
* @param	Index	Name index to look up string for
* @return			Associated name
*/
const TCHAR* DebugFName(FNameEntryId Index)
{
	// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
	static TCHAR TempName[NAME_SIZE];
	FCString::Strcpy(TempName, *FName::SafeString(Index));
	return TempName;
}

/**
* Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Class->Name.Index, Class->Name.Number)". 
*
* @param	Index	Name index to look up string for
* @param	Number	Internal instance number of the FName to print (which is 1 more than the printed number)
* @return			Associated name
*/
const TCHAR* DebugFName(int32 Index, int32 Number)
{
	// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
	static TCHAR TempName[NAME_SIZE];
	FCString::Strcpy(TempName, *FName::SafeString(DebugCastNameEntryId(Index), Number));
	return TempName;
}

/**
 * Helper function that can be used inside the debuggers watch window. E.g. "DebugFName(Class->Name)". 
 *
 * @param	Name	Name to look up string for
 * @return			Associated name
 */
const TCHAR* DebugFName(FName& Name)
{
	// Hardcoded static array. This function is only used inside the debugger so it should be fine to return it.
	static TCHAR TempName[NAME_SIZE];
	FCString::Strcpy(TempName, *FName::SafeString(Name.GetDisplayIndex(), Name.GetNumber()));
	return TempName;
}

template <typename TCharType>
static uint16 GetRawCasePreservingHash(const TCharType* Source)
{
	return FCrc::StrCrc32(Source) & 0xFFFF;

}
template <typename TCharType>
static uint16 GetRawNonCasePreservingHash(const TCharType* Source)
{
	return FCrc::Strihash_DEPRECATED(Source) & 0xFFFF;
}

/*-----------------------------------------------------------------------------
	FNameEntry
-----------------------------------------------------------------------------*/

void FNameEntry::StoreName(const ANSICHAR* InName, uint32 Len)
{
	FPlatformMemory::Memcpy(AnsiName, InName, sizeof(ANSICHAR) * Len);
	Encode(AnsiName, Len);
}

void FNameEntry::StoreName(const WIDECHAR* InName, uint32 Len)
{
	FPlatformMemory::Memcpy(WideName, InName, sizeof(WIDECHAR) * Len);
	Encode(WideName, Len);
}

void FNameEntry::CopyUnterminatedName(ANSICHAR* Out) const
{
	FPlatformMemory::Memcpy(Out, AnsiName, sizeof(ANSICHAR) * Header.Len);
	Decode(Out, Header.Len);
}

void FNameEntry::CopyUnterminatedName(WIDECHAR* Out) const
{
	FPlatformMemory::Memcpy(Out, WideName, sizeof(WIDECHAR) * Header.Len);
	Decode(Out, Header.Len);
}

FORCEINLINE const WIDECHAR* FNameEntry::GetUnterminatedName(WIDECHAR(&OptionalDecodeBuffer)[NAME_SIZE]) const
{
#ifdef WITH_CUSTOM_NAME_ENCODING
	CopyUnterminatedName(OptionalDecodeBuffer);
	return OptionalDecodeBuffer;
#else
	return WideName;
#endif
}

FORCEINLINE ANSICHAR const* FNameEntry::GetUnterminatedName(ANSICHAR(&OptionalDecodeBuffer)[NAME_SIZE]) const
{
#ifdef WITH_CUSTOM_NAME_ENCODING
	CopyUnterminatedName(OptionalDecodeBuffer);
	return OptionalDecodeBuffer;
#else
	return AnsiName;
#endif
}

FORCEINLINE FNameStringView FNameEntry::MakeView(FNameBuffer& OptionalDecodeBuffer) const
{
	return IsWide()	? FNameStringView(GetUnterminatedName(OptionalDecodeBuffer.WideName), GetNameLength())
					: FNameStringView(GetUnterminatedName(OptionalDecodeBuffer.AnsiName), GetNameLength());
}

void FNameEntry::GetUnterminatedName(TCHAR* OutName, uint32 OutLen) const
{
	check(static_cast<int32>(OutLen) >= GetNameLength());
	CopyAndConvertUnterminatedName(OutName);
}

void FNameEntry::GetName(TCHAR(&OutName)[NAME_SIZE]) const
{
	CopyAndConvertUnterminatedName(OutName);
	OutName[GetNameLength()] = '\0';
}

void FNameEntry::CopyAndConvertUnterminatedName(TCHAR* OutName) const
{
	if (sizeof(TCHAR) < sizeof(WIDECHAR) && IsWide()) // Normally compiled out
	{
		FNameBuffer Temp;
		CopyUnterminatedName(Temp.WideName);
		ConvertInPlace<WIDECHAR, TCHAR>(Temp.WideName, Header.Len);
		FPlatformMemory::Memcpy(OutName, Temp.AnsiName, Header.Len * sizeof(TCHAR));
	}
	else if (IsWide())
	{
		CopyUnterminatedName((WIDECHAR*)OutName);
		ConvertInPlace<WIDECHAR, TCHAR>((WIDECHAR*)OutName, Header.Len);
	}
	else
	{
		CopyUnterminatedName((ANSICHAR*)OutName);
		ConvertInPlace<ANSICHAR, TCHAR>((ANSICHAR*)OutName, Header.Len);
	}
}

void FNameEntry::GetAnsiName(ANSICHAR(&Out)[NAME_SIZE]) const
{
	check(!IsWide());
	CopyUnterminatedName(Out);
	Out[Header.Len] = '\0';
}

void FNameEntry::GetWideName(WIDECHAR(&Out)[NAME_SIZE]) const
{
	check(IsWide());
	CopyUnterminatedName(Out);
	Out[Header.Len] = '\0';
}

/** @return null-terminated string */
static const TCHAR* EntryToCString(const FNameEntry& Entry, FNameBuffer& Temp)
{
	if (Entry.IsWide())
	{
		Entry.GetWideName(Temp.WideName);
		return ConvertInPlace<WIDECHAR, TCHAR>(Temp.WideName, Entry.GetNameLength() + 1);
	}
	else
	{
		Entry.GetAnsiName(Temp.AnsiName);
		return ConvertInPlace<ANSICHAR, TCHAR>(Temp.AnsiName, Entry.GetNameLength() + 1);
	}
}

FString FNameEntry::GetPlainNameString() const
{
	FNameBuffer Temp;
	if (Header.bIsWide)
	{
		return FString(Header.Len, GetUnterminatedName(Temp.WideName));
	}
	else
	{
		return FString(Header.Len, GetUnterminatedName(Temp.AnsiName));
	}
}

void FNameEntry::AppendNameToString(FString& Out) const
{
	FNameBuffer Temp;
	Out.Append(EntryToCString(*this, Temp), Header.Len);
}

void FNameEntry::AppendNameToString(FStringBuilderBase& Out) const
{
	const int32 Offset = Out.AddUninitialized(Header.Len);
	TCHAR* OutChars = Out.GetData() + Offset;
	if (Header.bIsWide)
	{
		CopyUnterminatedName(reinterpret_cast<WIDECHAR*>(OutChars));
		ConvertInPlace<WIDECHAR, TCHAR>(reinterpret_cast<WIDECHAR*>(OutChars), Header.Len);
	}
	else
	{
		CopyUnterminatedName(reinterpret_cast<ANSICHAR*>(OutChars));
		ConvertInPlace<ANSICHAR, TCHAR>(reinterpret_cast<ANSICHAR*>(OutChars), Header.Len);
	}
}

void FNameEntry::AppendAnsiNameToString(FAnsiStringBuilderBase& Out) const
{
	check(!IsWide());
	const int32 Offset = Out.AddUninitialized(Header.Len);
	CopyUnterminatedName(Out.GetData() + Offset);
}

void FNameEntry::AppendNameToPathString(FString& Out) const
{
	FNameBuffer Temp;
	Out.PathAppend(EntryToCString(*this, Temp), Header.Len);
}

int32 FNameEntry::GetSize(const TCHAR* Name)
{
	return FNameEntry::GetSize(FCString::Strlen(Name), FCString::IsPureAnsi(Name));
}

int32 FNameEntry::GetSize(int32 Length, bool bIsPureAnsi)
{
	int32 Bytes = GetDataOffset() + Length * (bIsPureAnsi ? sizeof(ANSICHAR) : sizeof(WIDECHAR));
	return Align(Bytes, alignof(FNameEntry));
}

int32 FNameEntry::GetSizeInBytes() const
{
	return GetSize(GetNameLength(), !IsWide());
}

FNameEntrySerialized::FNameEntrySerialized(const FNameEntry& NameEntry)
{
	bIsWide = NameEntry.IsWide();
	if (bIsWide)
	{
		NameEntry.GetWideName(WideName);
		NonCasePreservingHash = GetRawNonCasePreservingHash(WideName);
		CasePreservingHash = GetRawCasePreservingHash(WideName);
	}
	else
	{
		NameEntry.GetAnsiName(AnsiName);
		NonCasePreservingHash = GetRawNonCasePreservingHash(AnsiName);
		CasePreservingHash = GetRawCasePreservingHash(AnsiName);
	}
}

/**
 * @return FString of name portion minus number.
 */
FString FNameEntrySerialized::GetPlainNameString() const
{
	if (bIsWide)
	{
		return FString(WideName);
	}
	else
	{
		return FString(AnsiName);
	}
}

/*-----------------------------------------------------------------------------
	FName statics.
-----------------------------------------------------------------------------*/

int32 FName::GetNameEntryMemorySize()
{
	return GetNamePool().NumBlocks() * FNameEntryAllocator::BlockSizeBytes;
}

int32 FName::GetNameTableMemorySize()
{
	return GetNameEntryMemorySize() + sizeof(FNamePool) + GetNamePool().NumSlots() * sizeof(FNameSlot);
}

int32 FName::GetNumAnsiNames()
{
	return GetNamePool().NumAnsiEntries();
}

int32 FName::GetNumWideNames()
{
	return GetNamePool().NumWideEntries();
}

TArray<const FNameEntry*> FName::DebugDump()
{
	return GetNamePool().DebugDump();
}

FNameEntry const* FName::GetEntry(EName Ename)
{
	FNamePool& Pool = GetNamePool();
	return &Pool.Resolve(Pool.Find(Ename));
}

FNameEntry const* FName::GetEntry(FNameEntryId Id)
{
	return &GetNamePool().Resolve(Id);
}

FString FName::NameToDisplayString( const FString& InDisplayName, const bool bIsBool )
{
	// Copy the characters out so that we can modify the string in place
	const TArray< TCHAR >& Chars = InDisplayName.GetCharArray();

	// This is used to indicate that we are in a run of uppercase letter and/or digits.  The code attempts to keep
	// these characters together as breaking them up often looks silly (i.e. "Draw Scale 3 D" as opposed to "Draw Scale 3D"
	bool bInARun = false;
	bool bWasSpace = false;
	bool bWasOpenParen = false;
	bool bWasNumber = false;
	bool bWasMinusSign = false;

	FString OutDisplayName;
	OutDisplayName.GetCharArray().Reserve(Chars.Num());
	for( int32 CharIndex = 0 ; CharIndex < Chars.Num() ; ++CharIndex )
	{
		TCHAR ch = Chars[CharIndex];

		bool bLowerCase = FChar::IsLower( ch );
		bool bUpperCase = FChar::IsUpper( ch );
		bool bIsDigit = FChar::IsDigit( ch );
		bool bIsUnderscore = FChar::IsUnderscore( ch );

		// Skip the first character if the property is a bool (they should all start with a lowercase 'b', which we don't want to keep)
		if( CharIndex == 0 && bIsBool && ch == 'b' )
		{
			// Check if next character is uppercase as it may be a user created string that doesn't follow the rules of Unreal variables
			if (Chars.Num() > 1 && FChar::IsUpper(Chars[1]))
			{
				continue;
			}
		}

		// If the current character is upper case or a digit, and the previous character wasn't, then we need to insert a space if there wasn't one previously
		// We don't do this for numerical expressions, for example "-1.2" should not be formatted as "- 1. 2"
		if( (bUpperCase || (bIsDigit && !bWasMinusSign)) && !bInARun && !bWasOpenParen && !bWasNumber)
		{
			if( !bWasSpace && OutDisplayName.Len() > 0 )
			{
				OutDisplayName += TEXT( ' ' );
				bWasSpace = true;
			}
			bInARun = true;
		}

		// A lower case character will break a run of upper case letters and/or digits
		if( bLowerCase )
		{
			bInARun = false;
		}

		// An underscore denotes a space, so replace it and continue the run
		if( bIsUnderscore )
		{
			ch = TEXT( ' ' );
			bInARun = true;
		}

		// If this is the first character in the string, then it will always be upper-case
		if( OutDisplayName.Len() == 0 )
		{
			ch = FChar::ToUpper( ch );
		}
		else if( !bIsDigit && (bWasSpace || bWasOpenParen))	// If this is first character after a space, then make sure it is case-correct
		{
			// Some words are always forced lowercase
			const TCHAR* Articles[] =
			{
				TEXT( "In" ),
				TEXT( "As" ),
				TEXT( "To" ),
				TEXT( "Or" ),
				TEXT( "At" ),
				TEXT( "On" ),
				TEXT( "If" ),
				TEXT( "Be" ),
				TEXT( "By" ),
				TEXT( "The" ),
				TEXT( "For" ),
				TEXT( "And" ),
				TEXT( "With" ),
				TEXT( "When" ),
				TEXT( "From" ),
			};

			// Search for a word that needs case repaired
			bool bIsArticle = false;
			for( int32 CurArticleIndex = 0; CurArticleIndex < UE_ARRAY_COUNT( Articles ); ++CurArticleIndex )
			{
				// Make sure the character following the string we're testing is not lowercase (we don't want to match "in" with "instance")
				const int32 ArticleLength = FCString::Strlen( Articles[ CurArticleIndex ] );
				if( ( Chars.Num() - CharIndex ) > ArticleLength && !FChar::IsLower( Chars[ CharIndex + ArticleLength ] ) && Chars[ CharIndex + ArticleLength ] != '\0' )
				{
					// Does this match the current article?
					if( FCString::Strncmp( &Chars[ CharIndex ], Articles[ CurArticleIndex ], ArticleLength ) == 0 )
					{
						bIsArticle = true;
						break;
					}
				}
			}

			// Start of a keyword, force to lowercase
			if( bIsArticle )
			{
				ch = FChar::ToLower( ch );				
			}
			else	// First character after a space that's not a reserved keyword, make sure it's uppercase
			{
				ch = FChar::ToUpper( ch );
			}
		}

		bWasSpace = ( ch == TEXT( ' ' ) ? true : false );
		bWasOpenParen = ( ch == TEXT( '(' ) ? true : false );

		// What could be included as part of a numerical representation.
		// For example -1.2
		bWasMinusSign = (ch == TEXT('-'));
		const bool bPotentialNumericalChar = bWasMinusSign || (ch == TEXT('.'));
		bWasNumber = bIsDigit || (bWasNumber && bPotentialNumericalChar);

		OutDisplayName += ch;
	}

	return OutDisplayName;
}

const EName* FName::ToEName() const
{
	return GetNamePoolPostInit().FindEName(ComparisonIndex);
}

bool FName::IsWithinBounds(FNameEntryId Id)
{
	return GetNamePoolPostInit().IsValid(Id);
}

/*-----------------------------------------------------------------------------
	FName implementation.
-----------------------------------------------------------------------------*/

template<class CharType>
static bool NumberEqualsString(uint32 Number, const CharType* Str)
{
	CharType* End = nullptr;
	return TCString<CharType>::Strtoi64(Str, &End, 10) == Number && End && *End == '\0';
}

template<class CharType1, class CharType2>
static bool StringAndNumberEqualsString(const CharType1* Name, uint32 NameLen, int32 InternalNumber, const CharType2* Str)
{
	if (FPlatformString::Strnicmp(Name, Str, NameLen))
	{
		return false;
	}

	if (InternalNumber == NAME_NO_NUMBER_INTERNAL)
	{
		return Str[NameLen] == '\0';
	}

	uint32 Number = NAME_INTERNAL_TO_EXTERNAL(InternalNumber);
	return Str[NameLen] == '_' && NumberEqualsString(Number, Str + NameLen + 1);
}

struct FNameAnsiStringView
{
	using CharType = ANSICHAR;

	const ANSICHAR* Str;
	int32 Len;
};

struct FWideStringViewWithWidth
{
	using CharType = WIDECHAR;

	const WIDECHAR* Str;
	int32 Len;
	bool bIsWide;
};

static FNameAnsiStringView MakeUnconvertedView(const ANSICHAR* Str, int32 Len)
{
	return { Str, Len };
}

static FNameAnsiStringView MakeUnconvertedView(const ANSICHAR* Str)
{
	return { Str, Str ? FCStringAnsi::Strlen(Str) : 0 };
}

static bool IsWide(const WIDECHAR* Str, const int32 Len)
{
	uint32 UserCharBits = 0;
	for (int32 I = 0; I < Len; ++I)
	{
		UserCharBits |= TChar<WIDECHAR>::ToUnsigned(Str[I]);
	}
	return UserCharBits & 0xffffff80u;
}

static int32 GetLengthAndWidth(const WIDECHAR* Str, bool& bOutIsWide)
{
	uint32 UserCharBits = 0;
	const WIDECHAR* It = Str;
	if (Str)
	{
		while (*It)
		{
			UserCharBits |= TChar<WIDECHAR>::ToUnsigned(*It);
			++It;
		}
	}

	bOutIsWide = UserCharBits & 0xffffff80u;

	return UE_PTRDIFF_TO_INT32(It - Str);
}

static FWideStringViewWithWidth MakeUnconvertedView(const WIDECHAR* Str, int32 Len)
{
	return { Str, Len, IsWide(Str, Len) };
}

static FWideStringViewWithWidth MakeUnconvertedView(const WIDECHAR* Str)
{
	FWideStringViewWithWidth View;
	View.Str = Str;
	View.Len = GetLengthAndWidth(Str, View.bIsWide);
	return View;
}

// @pre Str contains only digits and the number is smaller than int64 max
template<typename CharType>
static constexpr int64 Atoi64(const CharType* Str, int32 Len)
{
    int64 N = 0;
    for (int32 Idx = 0; Idx < Len; ++Idx)
    {
        N = 10 * N + Str[Idx] - '0';
    }

    return N;
}

/** Templated implementations of non-templated member functions, helps keep header clean */
struct FNameHelper
{
	template<typename ViewType>
	static FName MakeDetectNumber(ViewType View, EFindName FindType)
	{
		if (View.Len == 0)
		{
			return FName();
		}
		
		uint32 InternalNumber = ParseNumber(View.Str, /* may be shortened */ View.Len);
		return MakeWithNumber(View, FindType, InternalNumber);
	}

	template<typename CharType>
	static uint32 ParseNumber(const CharType* Name, int32& InOutLen)
	{
		const int32 Len = InOutLen;
		int32 Digits = 0;
		for (const CharType* It = Name + Len - 1; It >= Name && *It >= '0' && *It <= '9'; --It)
		{
			++Digits;
		}

		const CharType* FirstDigit = Name + Len - Digits;
		static constexpr int32 MaxDigitsInt32 = 10;
		if (Digits && Digits < Len && *(FirstDigit - 1) == '_' && Digits <= MaxDigitsInt32)
		{
			// check for the case where there are multiple digits after the _ and the first one
			// is a 0 ("Rocket_04"). Can't split this case. (So, we check if the first char
			// is not 0 or the length of the number is 1 (since ROcket_0 is valid)
			if (Digits == 1 || *FirstDigit != '0')
			{
				int64 Number = Atoi64(Name + Len - Digits, Digits);
				if (Number < MAX_int32)
				{
					InOutLen -= 1 + Digits;
					return static_cast<uint32>(NAME_EXTERNAL_TO_INTERNAL(Number));
				}
			}
		}

		return NAME_NO_NUMBER_INTERNAL;
	}

	static FName MakeWithNumber(FNameAnsiStringView	 View, EFindName FindType, int32 InternalNumber)
	{
		// Ignore the supplied number if the name string is empty
		// to keep the semantics of the old FName implementation
		if (View.Len == 0)
		{
			return FName();
		}

		return Make(FNameStringView(View.Str, View.Len), FindType, InternalNumber);
	}

	static FName MakeWithNumber(const FWideStringViewWithWidth View, EFindName FindType, int32 InternalNumber)
	{
		// Ignore the supplied number if the name string is empty
		// to keep the semantics of the old FName implementation
		if (View.Len == 0)
		{
			return FName();
		}

		// Convert to narrow if possible
		if (!View.bIsWide)
		{
			// Consider _mm_packus_epi16 or similar if this proves too slow
			ANSICHAR AnsiName[NAME_SIZE];
			for (int32 I = 0, Len = FMath::Min<int32>(View.Len, NAME_SIZE); I < Len; ++I)
			{
				AnsiName[I] = View.Str[I];
			}
			return Make(FNameStringView(AnsiName, View.Len), FindType, InternalNumber);
		}
		else
		{
			return Make(FNameStringView(View.Str, View.Len), FindType, InternalNumber);
		}
	}

	static FName Make(FNameStringView View, EFindName FindType, int32 InternalNumber)
	{
		if (View.Len >= NAME_SIZE)
		{
			checkf(false, TEXT("FName's %d max length exceeded. Got %d characters excluding null-terminator."), NAME_SIZE - 1, View.Len);
			return FName("ERROR_NAME_SIZE_EXCEEDED");
		}
		
		FNamePool& Pool = GetNamePool();

		FNameEntryId DisplayId, ComparisonId;
		if (FindType == FNAME_Add)
		{
			DisplayId = Pool.Store(View);
#if WITH_CASE_PRESERVING_NAME
			ComparisonId = Pool.Resolve(DisplayId).ComparisonId;
#else
			ComparisonId = DisplayId;
#endif
		}
		else if (FindType == FNAME_Find)
		{
			DisplayId = Pool.Find(View);
#if WITH_CASE_PRESERVING_NAME
			ComparisonId = DisplayId ? Pool.Resolve(DisplayId).ComparisonId : DisplayId;
#else
			ComparisonId = DisplayId;
#endif
		}
		else
		{
			check(FindType == FNAME_Replace_Not_Safe_For_Threading);

#if FNAME_WRITE_PROTECT_PAGES
			checkf(false, TEXT("FNAME_Replace_Not_Safe_For_Threading can't be used together with page protection."));
#endif
			DisplayId = Pool.Store(View);
#if WITH_CASE_PRESERVING_NAME
			ComparisonId = Pool.Resolve(DisplayId).ComparisonId;
#else
			ComparisonId = DisplayId;
#endif
			ReplaceName(Pool.Resolve(ComparisonId), View);
		}

		return FName(ComparisonId, DisplayId, InternalNumber);
	}

	static FName MakeFromLoaded(const FNameEntrySerialized& LoadedEntry)
	{
		FNameStringView View = LoadedEntry.bIsWide
			? FNameStringView(LoadedEntry.WideName, FCStringWide::Strlen(LoadedEntry.WideName))
			: FNameStringView(LoadedEntry.AnsiName, FCStringAnsi::Strlen(LoadedEntry.AnsiName));

		return Make(View, FNAME_Add, NAME_NO_NUMBER_INTERNAL);
	}

	template<class CharType>
	static bool EqualsString(FName Name, const CharType* Str)
	{
		// Make NAME_None == TEXT("") or nullptr consistent with NAME_None == FName(TEXT("")) or FName(nullptr)
		if (Str == nullptr || Str[0] == '\0')
		{
			return Name.IsNone();
		}

		const FNameEntry& Entry = *Name.GetComparisonNameEntry();

		uint32 NameLen = Entry.Header.Len;
		FNameBuffer Temp;
		return Entry.IsWide()
			? StringAndNumberEqualsString(Entry.GetUnterminatedName(Temp.WideName), NameLen, Name.GetNumber(), Str)
			: StringAndNumberEqualsString(Entry.GetUnterminatedName(Temp.AnsiName), NameLen, Name.GetNumber(), Str);
	}

	static void ReplaceName(FNameEntry& Existing, FNameStringView Updated)
	{
		check(Existing.Header.bIsWide == Updated.bIsWide);
		check(Existing.Header.Len == Updated.Len);

		if (Updated.bIsWide)
		{
			Existing.StoreName(Updated.Wide, Updated.Len);
		}
		else
		{
			Existing.StoreName(Updated.Ansi, Updated.Len);
		}
	}
};


#if WITH_CASE_PRESERVING_NAME
FNameEntryId FName::GetComparisonIdFromDisplayId(FNameEntryId DisplayId)
{
	return GetEntry(DisplayId)->ComparisonId;
}
#endif

FName::FName(const WIDECHAR* Name, EFindName FindType)
	: FName(FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name), FindType))
{}

FName::FName(const ANSICHAR* Name, EFindName FindType)
	: FName(FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name), FindType))
{}

FName::FName(int32 Len, const WIDECHAR* Name, EFindName FindType)
	: FName(FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name, Len), FindType))
{}

FName::FName(int32 Len, const ANSICHAR* Name, EFindName FindType)
	: FName(FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name, Len), FindType))
{}

FName::FName(const WIDECHAR* Name, int32 InNumber, EFindName FindType)
	: FName(FNameHelper::MakeWithNumber(MakeUnconvertedView(Name), FindType, InNumber))
{}

FName::FName(const ANSICHAR* Name, int32 InNumber, EFindName FindType)
	: FName(FNameHelper::MakeWithNumber(MakeUnconvertedView(Name), FindType, InNumber))
{}

FName::FName(int32 Len, const WIDECHAR* Name, int32 InNumber, EFindName FindType)
	: FName(InNumber != NAME_NO_NUMBER_INTERNAL ? FNameHelper::MakeWithNumber(MakeUnconvertedView(Name, Len), FindType, InNumber)
												: FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name, Len), FindType))
{}

FName::FName(int32 Len, const ANSICHAR* Name, int32 InNumber, EFindName FindType)
	: FName(InNumber != NAME_NO_NUMBER_INTERNAL ? FNameHelper::MakeWithNumber(MakeUnconvertedView(Name, Len), FindType, InNumber)
												: FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name, Len), FindType))
{}

FName::FName(const TCHAR* Name, int32 InNumber, EFindName FindType, bool bSplitName)
	: FName(InNumber == NAME_NO_NUMBER_INTERNAL && bSplitName 
			? FNameHelper::MakeDetectNumber(MakeUnconvertedView(Name), FindType)
			: FNameHelper::MakeWithNumber(MakeUnconvertedView(Name), FindType, InNumber))
{}

FName::FName(const FNameEntrySerialized& LoadedEntry)
	: FName(FNameHelper::MakeFromLoaded(LoadedEntry))
{}

bool FName::operator==(const ANSICHAR* Str) const
{
	return FNameHelper::EqualsString(*this, Str);
}

bool FName::operator==(const WIDECHAR* Str) const
{
	return FNameHelper::EqualsString(*this, Str);
}

int32 FName::Compare( const FName& Other ) const
{
	// Names match, check whether numbers match.
	if (ComparisonIndex == Other.ComparisonIndex)
	{
		return GetNumber() - Other.GetNumber();
	}

	// Names don't match. This means we don't even need to check numbers.
	return CompareDifferentIdsAlphabetically(ComparisonIndex, Other.ComparisonIndex);
}

uint32 FName::GetPlainNameString(TCHAR(&OutName)[NAME_SIZE]) const
{
	const FNameEntry& Entry = *GetDisplayNameEntry();
	Entry.GetName(OutName);
	return Entry.GetNameLength();
}

FString FName::GetPlainNameString() const
{
	return GetDisplayNameEntry()->GetPlainNameString();
}

void FName::GetPlainANSIString(ANSICHAR(&AnsiName)[NAME_SIZE]) const
{
	GetDisplayNameEntry()->GetAnsiName(AnsiName);
}

void FName::GetPlainWIDEString(WIDECHAR(&WideName)[NAME_SIZE]) const
{
	GetDisplayNameEntry()->GetWideName(WideName);
}

const FNameEntry* FName::GetComparisonNameEntry() const
{
	return &GetNamePool().Resolve(GetComparisonIndex());
}

const FNameEntry* FName::GetDisplayNameEntry() const
{
	return &GetNamePool().Resolve(GetDisplayIndex());
}

FString FName::ToString() const
{
	if (GetNumber() == NAME_NO_NUMBER_INTERNAL)
	{
		// Avoids some extra allocations in non-number case
		return GetDisplayNameEntry()->GetPlainNameString();
	}
	
	FString Out;	
	ToString(Out);
	return Out;
}

void FName::ToString(FString& Out) const
{
	// A version of ToString that saves at least one string copy
	const FNameEntry* const NameEntry = GetDisplayNameEntry();

	if (GetNumber() == NAME_NO_NUMBER_INTERNAL)
	{
		Out.Empty(NameEntry->GetNameLength());
		NameEntry->AppendNameToString(Out);
	}	
	else
	{
		Out.Empty(NameEntry->GetNameLength() + 6);
		NameEntry->AppendNameToString(Out);

		Out += TEXT('_');
		Out.AppendInt(NAME_INTERNAL_TO_EXTERNAL(GetNumber()));
	}
}

void FName::ToString(FStringBuilderBase& Out) const
{
	Out.Reset();
	AppendString(Out);
}

uint32 FName::GetStringLength() const
{
	const FNameEntry& Entry = *GetDisplayNameEntry();
	uint32 NameLen = Entry.GetNameLength();

	if (GetNumber() == NAME_NO_NUMBER_INTERNAL)
	{
		return NameLen;
	}
	else
	{
		TCHAR NumberSuffixStr[16];
		int32 SuffixLen = FCString::Sprintf(NumberSuffixStr, TEXT("_%d"), NAME_INTERNAL_TO_EXTERNAL(GetNumber()));
		check(SuffixLen > 0);

		return NameLen + SuffixLen;
	}
}

uint32 FName::ToString(TCHAR* Out, uint32 OutSize) const
{
	const FNameEntry& Entry = *GetDisplayNameEntry();
	uint32 NameLen = Entry.GetNameLength();
	Entry.GetUnterminatedName(Out, OutSize);

	if (GetNumber() == NAME_NO_NUMBER_INTERNAL)
	{
		Out[NameLen] = '\0';
		return NameLen;
	}
	else
	{
		TCHAR NumberSuffixStr[16];
		int32 SuffixLen = FCString::Sprintf(NumberSuffixStr, TEXT("_%d"), NAME_INTERNAL_TO_EXTERNAL(GetNumber()));
		uint32 TotalLen = NameLen + SuffixLen;
		check(SuffixLen > 0 && OutSize > TotalLen);

		FPlatformMemory::Memcpy(Out + NameLen, NumberSuffixStr, SuffixLen * sizeof(TCHAR));
		Out[TotalLen] = '\0';
		return TotalLen;
	}
}

void FName::AppendString(FString& Out) const
{
	const FNameEntry* const NameEntry = GetDisplayNameEntry();

	NameEntry->AppendNameToString( Out );
	if (GetNumber() != NAME_NO_NUMBER_INTERNAL)
	{
		Out += TEXT('_');
		Out.AppendInt(NAME_INTERNAL_TO_EXTERNAL(GetNumber()));
	}
}

void FName::AppendString(FStringBuilderBase& Out) const
{
	GetDisplayNameEntry()->AppendNameToString(Out);

	const int32 InternalNumber = GetNumber();
	if (InternalNumber != NAME_NO_NUMBER_INTERNAL)
	{
		Out << TEXT('_') << NAME_INTERNAL_TO_EXTERNAL(InternalNumber);
	}
}

bool FName::TryAppendAnsiString(FAnsiStringBuilderBase& Out) const
{
	const FNameEntry* const NameEntry = GetDisplayNameEntry();

	if (NameEntry->IsWide())
	{
		return false;
	}

	NameEntry->AppendAnsiNameToString(Out);

	const int32 InternalNumber = GetNumber();
	if (InternalNumber != NAME_NO_NUMBER_INTERNAL)
	{
		Out << '_' << NAME_INTERNAL_TO_EXTERNAL(InternalNumber);
	}

	return true;
}

void FName::DisplayHash(FOutputDevice& Ar)
{
	GetNamePool().LogStats(Ar);
}

FString FName::SafeString(FNameEntryId InDisplayIndex, int32 InstanceNumber)
{
	return FName(InDisplayIndex, InDisplayIndex, InstanceNumber).ToString();
}

bool FName::IsValidXName(const FName InName, const FString& InInvalidChars, FText* OutReason, const FText* InErrorCtx)
{
	TStringBuilder<FName::StringBufferSize> NameStr;
	InName.ToString(NameStr);
	return IsValidXName(FStringView(NameStr), InInvalidChars, OutReason, InErrorCtx);
}

bool FName::IsValidXName(const TCHAR* InName, const FString& InInvalidChars, FText* OutReason, const FText* InErrorCtx)
{
	return IsValidXName(FStringView(InName), InInvalidChars, OutReason, InErrorCtx);
}

bool FName::IsValidXName(const FString& InName, const FString& InInvalidChars, FText* OutReason, const FText* InErrorCtx)
{
	return IsValidXName(FStringView(InName), InInvalidChars, OutReason, InErrorCtx);
}

bool FName::IsValidXName(const FStringView& InName, const FString& InInvalidChars, FText* OutReason, const FText* InErrorCtx)
{
	if (InName.IsEmpty() || InInvalidChars.IsEmpty())
	{
		return true;
	}

	// See if the name contains invalid characters.
	FString MatchedInvalidChars;
	TSet<TCHAR> AlreadyMatchedInvalidChars;
	for (const TCHAR InvalidChar : InInvalidChars)
	{
		int32 InvalidCharIndex = INDEX_NONE;
		if (!AlreadyMatchedInvalidChars.Contains(InvalidChar) && InName.FindChar(InvalidChar, InvalidCharIndex))
		{
			MatchedInvalidChars.AppendChar(InvalidChar);
			AlreadyMatchedInvalidChars.Add(InvalidChar);
		}
	}

	if (MatchedInvalidChars.Len())
	{
		if (OutReason)
		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("ErrorCtx"), (InErrorCtx) ? *InErrorCtx : NSLOCTEXT("Core", "NameDefaultErrorCtx", "Name"));
			Args.Add(TEXT("IllegalNameCharacters"), FText::FromString(MatchedInvalidChars));
			*OutReason = FText::Format(NSLOCTEXT("Core", "NameContainsInvalidCharacters", "{ErrorCtx} may not contain the following characters: {IllegalNameCharacters}"), Args);
		}
		return false;
	}

	return true;
}

FStringBuilderBase& operator<<(FStringBuilderBase& Builder, FNameEntryId Id)
{
	FName::GetEntry(Id)->AppendNameToString(Builder);
	return Builder;
}

template <typename CharType, int N>
void CheckLazyName(const CharType(&Literal)[N])
{
	check(FName(Literal) == FLazyName(Literal));
	check(FLazyName(Literal) == FName(Literal));
	check(FLazyName(Literal) == FLazyName(Literal));
	check(FName(Literal) == FLazyName(Literal).Resolve());

	CharType Literal2[N];
	FMemory::Memcpy(Literal2, Literal);
	check(FLazyName(Literal) == FLazyName(Literal2));
}

static void TestNameBatch();

void FName::AutoTest()
{
#if DO_CHECK
	check(FNameHash::IsAnsiNone("None", 4) == 1);
	check(FNameHash::IsAnsiNone("none", 4) == 1);
	check(FNameHash::IsAnsiNone("NONE", 4) == 1);
	check(FNameHash::IsAnsiNone("nOnE", 4) == 1);
	check(FNameHash::IsAnsiNone("None", 5) == 0);
	check(FNameHash::IsAnsiNone(TEXT("None"), 4) == 0);
	check(FNameHash::IsAnsiNone("nono", 4) == 0);
	check(FNameHash::IsAnsiNone("enon", 4) == 0);

	const FName AutoTest_1("AutoTest_1");
	const FName autoTest_1("autoTest_1");
	const FName autoTeSt_1("autoTeSt_1");
	const FName AutoTest_2(TEXT("AutoTest_2"));
	const FName AutoTestB_2(TEXT("AutoTestB_2"));

	check(AutoTest_1 != AutoTest_2);
	check(AutoTest_1 == autoTest_1);
	check(AutoTest_1 == autoTeSt_1);

	TCHAR Buffer[FName::StringBufferSize];

#if WITH_CASE_PRESERVING_NAME
	check(!FCString::Strcmp(*AutoTest_1.ToString(), TEXT("AutoTest_1")));
	check(!FCString::Strcmp(*autoTest_1.ToString(), TEXT("autoTest_1")));
	check(!FCString::Strcmp(*autoTeSt_1.ToString(), TEXT("autoTeSt_1")));
	check(!FCString::Strcmp(*AutoTestB_2.ToString(), TEXT("AutoTestB_2")));
	
	check(FName("ABC").ToString(Buffer) == 3 &&			!FCString::Strcmp(Buffer, TEXT("ABC")));
	check(FName("abc").ToString(Buffer) == 3 &&			!FCString::Strcmp(Buffer, TEXT("abc")));
	check(FName(TEXT("abc")).ToString(Buffer) == 3 &&	!FCString::Strcmp(Buffer, TEXT("abc")));
	check(FName("ABC_0").ToString(Buffer) == 5 &&		!FCString::Strcmp(Buffer, TEXT("ABC_0")));
	check(FName("ABC_10").ToString(Buffer) == 6 &&		!FCString::Strcmp(Buffer, TEXT("ABC_10")));	
#endif

	check(autoTest_1.GetComparisonIndex() == AutoTest_2.GetComparisonIndex());
	check(autoTest_1.GetPlainNameString() == AutoTest_1.GetPlainNameString());
	check(autoTest_1.GetPlainNameString() == AutoTest_2.GetPlainNameString());
	check(*AutoTestB_2.GetPlainNameString() != *AutoTest_2.GetPlainNameString());
	check(AutoTestB_2.GetNumber() == AutoTest_2.GetNumber());
	check(autoTest_1.GetNumber() != AutoTest_2.GetNumber());

	check(FCStringAnsi::Strlen("None") == FName().GetStringLength());
	check(FCStringAnsi::Strlen("ABC") == FName("ABC").GetStringLength());
	check(FCStringAnsi::Strlen("ABC_0") == FName("ABC_0").GetStringLength());
	check(FCStringAnsi::Strlen("ABC_9") == FName("ABC_9").GetStringLength());
	check(FCStringAnsi::Strlen("ABC_10") == FName("ABC_10").GetStringLength());
	check(FCStringAnsi::Strlen("ABC_2000000000") == FName("ABC_2000000000").GetStringLength());
	check(FCStringAnsi::Strlen("ABC_4000000000") == FName("ABC_4000000000").GetStringLength());

	const FName NullName(static_cast<ANSICHAR*>(nullptr));
	check(NullName.IsNone());
	check(NullName == FName(static_cast<WIDECHAR*>(nullptr)));
	check(NullName == FName(NAME_None));
	check(NullName == FName());
	check(NullName == FName(""));
	check(NullName == FName(TEXT("")));
	check(NullName == FName("None"));
	check(NullName == FName("none"));
	check(NullName == FName("NONE"));
	check(NullName == FName(TEXT("None")));
	check(FName().ToEName());
	check(*FName().ToEName() == NAME_None);
	check(NullName.GetComparisonIndex().ToUnstableInt() == 0);

	const FName Cylinder(NAME_Cylinder);
	check(Cylinder == FName("Cylinder"));
	check(Cylinder.ToEName());
	check(*Cylinder.ToEName() == NAME_Cylinder);
	check(Cylinder.GetPlainNameString() == TEXT("Cylinder"));

	// Test numbers
	check(FName("Text_0") == FName("Text", NAME_EXTERNAL_TO_INTERNAL(0)));
	check(FName("Text_1") == FName("Text", NAME_EXTERNAL_TO_INTERNAL(1)));
	check(FName("Text_1_0") == FName("Text_1", NAME_EXTERNAL_TO_INTERNAL(0)));
	check(FName("Text_0_1") == FName("Text_0", NAME_EXTERNAL_TO_INTERNAL(1)));
	check(FName("Text_00") == FName("Text_00", NAME_NO_NUMBER_INTERNAL));
	check(FName("Text_01") == FName("Text_01", NAME_NO_NUMBER_INTERNAL));

	// Test unterminated strings
	check(FName("") == FName(0, "Unused"));
	check(FName("Used") == FName(4, "UsedUnused"));
	check(FName("Used") == FName(4, "Used"));
	check(FName("Used_0") == FName(6, "Used_01"));
	check(FName("Used_01") == FName(7, "Used_012"));
	check(FName("Used_123") == FName(8, "Used_123456"));
	check(FName("Used_123") == FName(8, "Used_123_456"));
	check(FName("Used_123") == FName(8, TEXT("Used_123456")));
	check(FName("Used_123") == FName(8, TEXT("Used_123_456")));
	check(FName("Used_2147483646") == FName(15, TEXT("Used_2147483646123")));
	check(FName("Used_2147483647") == FName(15, TEXT("Used_2147483647123")));
	check(FName("Used_2147483648") == FName(15, TEXT("Used_2147483648123")));

	// Test wide strings
	FString Wide("Wide ");
	Wide[4] = 60000;
	FName WideName(*Wide);
	check(WideName.GetPlainNameString() == Wide);
	check(FName(*Wide).GetPlainNameString() == Wide);
	check(FName(*Wide).ToString(Buffer) == 5 && !FCString::Strcmp(Buffer, *Wide));
	check(Wide.Len() == WideName.GetStringLength());
	FString WideLong = FString::ChrN(1000, 60000);
	check(FName(*WideLong).GetPlainNameString() == WideLong);


	// Check that FNAME_Find doesn't add entries
	static bool Once = true;
	if (Once)
	{
		check(FName("UniqueUnicorn!!", FNAME_Find) == FName());

		// Check that FNAME_Find can find entries
		const FName UniqueName("UniqueUnicorn!!", FNAME_Add);
		check(FName("UniqueUnicorn!!", FNAME_Find) == UniqueName);
		check(FName(TEXT("UniqueUnicorn!!"), FNAME_Find) == UniqueName);
		check(FName("UNIQUEUNICORN!!", FNAME_Find) == UniqueName);
		check(FName(TEXT("UNIQUEUNICORN!!"), FNAME_Find) == UniqueName);
		check(FName("uniqueunicorn!!", FNAME_Find) == UniqueName);

#if !FNAME_WRITE_PROTECT_PAGES
		// Check FNAME_Replace_Not_Safe_For_Threading updates casing
		check(0 != UniqueName.GetPlainNameString().Compare("UNIQUEunicorn!!", ESearchCase::CaseSensitive));
		const FName UniqueNameReplaced("UNIQUEunicorn!!", FNAME_Replace_Not_Safe_For_Threading);
		check(0 == UniqueName.GetPlainNameString().Compare("UNIQUEunicorn!!", ESearchCase::CaseSensitive));
		check(UniqueNameReplaced == UniqueName);

		// Check FNAME_Replace_Not_Safe_For_Threading works with wide string
		check(0 != UniqueName.GetPlainNameString().Compare("uniqueunicorn!!", ESearchCase::CaseSensitive));
		const FName UpdatedCasing(TEXT("uniqueunicorn!!"), FNAME_Replace_Not_Safe_For_Threading);
		check(0 == UniqueName.GetPlainNameString().Compare("uniqueunicorn!!", ESearchCase::CaseSensitive));

		// Check FNAME_Replace_Not_Safe_For_Threading adds entries that do not exist
		const FName AddedByReplace("WasAdded!!", FNAME_Replace_Not_Safe_For_Threading);
		check(FName("WasAdded!!", FNAME_Find) == AddedByReplace);
#endif
	
		Once = false;
	}

	check(NumberEqualsString(0, "0"));
	check(NumberEqualsString(11, "11"));
	check(NumberEqualsString(2147483647, "2147483647"));
	check(NumberEqualsString(4294967294, "4294967294"));

	check(!NumberEqualsString(0, "1"));
	check(!NumberEqualsString(1, "0"));
	check(!NumberEqualsString(11, "12"));
	check(!NumberEqualsString(12, "11"));
	check(!NumberEqualsString(2147483647, "2147483646"));
	check(!NumberEqualsString(2147483646, "2147483647"));

	check(StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(10), "abc_10"));
	check(!StringAndNumberEqualsString("aba", 3, NAME_EXTERNAL_TO_INTERNAL(10), "abc_10"));
	check(!StringAndNumberEqualsString("abc", 2, NAME_EXTERNAL_TO_INTERNAL(10), "abc_10"));
	check(!StringAndNumberEqualsString("abc", 2, NAME_EXTERNAL_TO_INTERNAL(11), "abc_10"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(10), "aba_10"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(10), "abc_11"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(10), "abc_100"));

	check(StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(0), "abc_0"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_EXTERNAL_TO_INTERNAL(0), "abc_1"));

	check(StringAndNumberEqualsString("abc", 3, NAME_NO_NUMBER_INTERNAL, "abc"));
	check(!StringAndNumberEqualsString("abc", 2, NAME_NO_NUMBER_INTERNAL, "abc"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_NO_NUMBER_INTERNAL, "abcd"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_NO_NUMBER_INTERNAL, "abc_0"));
	check(!StringAndNumberEqualsString("abc", 3, NAME_NO_NUMBER_INTERNAL, "abc_"));

	TArray<FName> Names;
	Names.Add("FooB");
	Names.Add("FooABCD");
	Names.Add("FooABC");
	Names.Add("FooAB");
	Names.Add("FooA");
	Names.Add("FooC");
	const WIDECHAR FooWide[] = {'F', 'o', 'o', (WIDECHAR)2000, '\0'};
	Names.Add(FooWide);
	Algo::Sort(Names, FNameLexicalLess()); 

	check(Names[0] == "FooA");
	check(Names[1] == "FooAB");
	check(Names[2] == "FooABC");
	check(Names[3] == "FooABCD");
	check(Names[4] == "FooB");
	check(Names[5] == "FooC");
	check(Names[6] == FooWide);

	
	CheckLazyName("Hej");
	CheckLazyName(TEXT("Hej"));
	CheckLazyName("Hej_0");
	CheckLazyName("Hej_00");
	CheckLazyName("Hej_1");
	CheckLazyName("Hej_01");
	CheckLazyName("Hej_-1");
	CheckLazyName("Hej__0");
	CheckLazyName("Hej_2147483647");
	CheckLazyName("Hej_123");
	CheckLazyName("None");
	CheckLazyName("none");
	CheckLazyName("None_0");
	CheckLazyName("None_1");

	TestNameBatch();

#if 0
	// Check hash table growth still yields the same unique FName ids
	static int32 OverflowAtLeastTwiceCount = 4 * FNamePoolInitialSlotsPerShard * FNamePoolShards;
	TArray<FNameEntryId> Ids;
	for (int I = 0; I < OverflowAtLeastTwiceCount; ++I)
	{
		FNameEntryId Id = FName(*FString::Printf(TEXT("%d"), I)).GetComparisonIndex();
		Ids.Add(Id);
	}

	for (int I = 0; I < OverflowAtLeastTwiceCount; ++I)
	{
		FNameEntryId Id = FName(*FString::Printf(TEXT("%d"), I)).GetComparisonIndex();
		FNameEntryId OldId = Ids[I];

		while (Id != OldId)
		{
			Id = FName(*FString::Printf(TEXT("%d"), I)).GetComparisonIndex();
		}
		check(Id == OldId);
	}
#endif
#endif // DO_CHECK
}


/*-----------------------------------------------------------------------------
	FNameEntry implementation.
-----------------------------------------------------------------------------*/

void FNameEntry::Write( FArchive& Ar ) const
{
	// This path should be unused - since FNameEntry structs are allocated with a dynamic size, we can only save them. Use FNameEntrySerialized to read them back into an intermediate buffer.
	checkf(!Ar.IsLoading(), TEXT("FNameEntry does not support reading from an archive. Serialize into a FNameEntrySerialized and construct a FNameEntry from that."));

	// Convert to our serialized type
	FNameEntrySerialized EntrySerialized(*this);
	Ar << EntrySerialized;
}

static_assert(PLATFORM_LITTLE_ENDIAN, "FNameEntrySerialized serialization needs updating to support big-endian platforms!");

FArchive& operator<<(FArchive& Ar, FNameEntrySerialized& E)
{
	if (Ar.IsLoading())
	{
		// for optimization reasons, we want to keep pure Ansi strings as Ansi for initializing the name entry
		// (and later the FName) to stop copying in and out of TCHARs
		int32 StringLen;
		Ar << StringLen;

		// negative stringlen means it's a wide string
		if (StringLen < 0)
		{
			// If StringLen cannot be negated due to integer overflow, Ar is corrupted.
			if (StringLen == MIN_int32)
			{
				Ar.SetCriticalError();
				UE_LOG(LogUnrealNames, Error, TEXT("Archive is corrupted"));
				return Ar;
			}

			StringLen = -StringLen;

			int64 MaxSerializeSize = Ar.GetMaxSerializeSize();
			// Protect against network packets allocating too much memory
			if ((MaxSerializeSize > 0) && (StringLen > MaxSerializeSize))
			{
				Ar.SetCriticalError();
				UE_LOG(LogUnrealNames, Error, TEXT("String is too large"));
				return Ar;
			}

			// mark the name will be wide
			E.bIsWide = true;

			// get the pointer to the wide array 
			WIDECHAR* WideName = const_cast<WIDECHAR*>(E.GetWideName());

			// read in the UCS2CHAR string
			auto Sink = StringMemoryPassthru<UCS2CHAR>(WideName, StringLen, StringLen);
			Ar.Serialize(Sink.Get(), StringLen * sizeof(UCS2CHAR));
			Sink.Apply();

#if PLATFORM_TCHAR_IS_4_BYTES
			// Inline combine any surrogate pairs in the data when loading into a UTF-32 string
			StringLen = StringConv::InlineCombineSurrogates_Buffer(WideName, StringLen);
#endif	// PLATFORM_TCHAR_IS_4_BYTES
		}
		else
		{
			int64 MaxSerializeSize = Ar.GetMaxSerializeSize();
			// Protect against network packets allocating too much memory
			if ((MaxSerializeSize > 0) && (StringLen > MaxSerializeSize))
			{
				Ar.SetCriticalError();
				UE_LOG(LogUnrealNames, Error, TEXT("String is too large"));
				return Ar;
			}

			// mark the name will be ansi
			E.bIsWide = false;

			// ansi strings can go right into the AnsiBuffer
			ANSICHAR* AnsiName = const_cast<ANSICHAR*>(E.GetAnsiName());
			Ar.Serialize(AnsiName, StringLen);
		}

		uint16 DummyHashes[2];
		uint32 SkipPastHashBytes = (Ar.UE4Ver() >= VER_UE4_NAME_HASHES_SERIALIZED) * sizeof(DummyHashes);
		Ar.Serialize(&DummyHashes, SkipPastHashBytes);
	}
	else
	{
		// These hashes are no longer used. They're only kept to maintain serialization format.
		// Please remove them if you ever change serialization format.
		FString Str = E.GetPlainNameString();
		Ar << Str;
		Ar << E.NonCasePreservingHash;
		Ar << E.CasePreservingHash;
	}

	return Ar;
}

FNameEntryId FNameEntryId::FromValidEName(EName Ename)
{
	return GetNamePool().Find(Ename);
}


void FName::TearDown()
{
	check(IsInGameThread());

	if (bNamePoolInitialized)
	{
		GetNamePoolPostInit().~FNamePool();
		bNamePoolInitialized = false;
	}
}

FName FLazyName::Resolve() const
{
	// Make a stack copy to ensure thread-safety
	FLiteralOrName Copy = Either;

	if (Copy.IsName())
	{
		FNameEntryId Id = Copy.AsName();
		return FName(Id, Id, Number);
	}

	// Resolve to FName but throw away the number part
	FNameEntryId Id = bLiteralIsWide ? FName(Copy.AsWideLiteral()).GetComparisonIndex()
										: FName(Copy.AsAnsiLiteral()).GetComparisonIndex();

	// Deliberately unsynchronized write of word-sized int, ok if multiple threads resolve same lazy name
	Either = FLiteralOrName(Id);

	return FName(Id, Id, Number);		
}

uint32 FLazyName::ParseNumber(const ANSICHAR* Str, int32 Len)
{
	return FNameHelper::ParseNumber(Str, Len);
}

uint32 FLazyName::ParseNumber(const WIDECHAR* Str, int32 Len)
{
	return FNameHelper::ParseNumber(Str, Len);
}

bool operator==(const FLazyName& A, const FLazyName& B)
{
	// If we have started creating FNames we might as well resolve and cache both lazy names
	if (A.Either.IsName() || B.Either.IsName())
	{
		return A.Resolve() == B.Resolve();
	}

	// Literal pointer comparison, can ignore width
	if (A.Either.AsAnsiLiteral() == B.Either.AsAnsiLiteral())
	{
		return true;
	}

	if (A.bLiteralIsWide)
	{
		return B.bLiteralIsWide ? FPlatformString::Stricmp(A.Either.AsWideLiteral(), B.Either.AsWideLiteral()) == 0
								: FPlatformString::Stricmp(A.Either.AsWideLiteral(), B.Either.AsAnsiLiteral()) == 0;
	}
	else
	{
		return B.bLiteralIsWide ? FPlatformString::Stricmp(A.Either.AsAnsiLiteral(), B.Either.AsWideLiteral()) == 0
								: FPlatformString::Stricmp(A.Either.AsAnsiLiteral(), B.Either.AsAnsiLiteral()) == 0;	
	}
}

/*-----------------------------------------------------------------------------
	FName batch serialization
-----------------------------------------------------------------------------*/

/**
 * FNameStringView sibling with UTF16 Little-Endian wide strings instead of WIDECHAR 
 *
 * View into serialized data instead of how it will be stored in memory once loaded.
 */
struct FNameSerializedView
{
	FNameSerializedView(const ANSICHAR* InStr, uint32 InLen)
	: Ansi(InStr)
	, Len(InLen)
	, bIsUtf16(false)
	{}
	
	FNameSerializedView(const UTF16CHAR* InStr, uint32 InLen)
	: Utf16(InStr)
	, Len(InLen)
	, bIsUtf16(true)
	{}

	FNameSerializedView(const uint8* InData, uint32 InLen, bool bInUtf16)
	: Data(InData)
	, Len(InLen)
	, bIsUtf16(bInUtf16)
	{}

	union
	{
		const uint8* Data;
		const ANSICHAR* Ansi;
		const UTF16CHAR* Utf16;
	};

	uint32 Len;
	bool bIsUtf16;
};

static uint8* AddUninitializedBytes(TArray<uint8>& Out, uint32 Bytes)
{
	uint32 OldNum = Out.AddUninitialized(Bytes);
	return Out.GetData() + OldNum;
}

template<typename T>
static T* AddUninitializedElements(TArray<uint8>& Out, uint32 Num)
{
	check(Out.Num() %  alignof(T) == 0);
	return reinterpret_cast<T*>(AddUninitializedBytes(Out, Num * sizeof(T)));
}

template<typename T>
static void AddValue(TArray<uint8>& Out, T Value)
{
	*AddUninitializedElements<T>(Out, 1) = Value;
}

template<typename T>
static void AlignTo(TArray<uint8>& Out)
{
	if (uint32 UnpaddedBytes = Out.Num() % sizeof(T))
	{
		Out.AddZeroed(sizeof(T) - UnpaddedBytes);
	}
}

static uint32 GetRequiredUtf16Padding(const uint8* Ptr)
{
	return UPTRINT(Ptr) & 1u;
}

struct FSerializedNameHeader
{
	FSerializedNameHeader(uint32 Len, bool bIsUtf16)
	{
		static_assert(NAME_SIZE < 0x8000u, "");
		check(Len <= NAME_SIZE);

		Data[0] = uint8(bIsUtf16) << 7 | static_cast<uint8>(Len >> 8);
		Data[1] = static_cast<uint8>(Len);
	}

	uint8 IsUtf16() const
	{
		return Data[0] & 0x80u;
	}

	uint32 Len() const
	{
		return ((Data[0] & 0x7Fu) << 8) + Data[1];
	}

	uint8 Data[2];
};

FNameSerializedView LoadNameHeader(const uint8*& InOutIt)
{
	const FSerializedNameHeader& Header = *reinterpret_cast<const FSerializedNameHeader*>(InOutIt);
	const uint8* NameData = InOutIt + sizeof(FSerializedNameHeader);
	const uint32 Len = Header.Len();

	if (Header.IsUtf16())
	{
		NameData += GetRequiredUtf16Padding(NameData);
		InOutIt = NameData + Len * sizeof(UTF16CHAR);
		return FNameSerializedView(NameData, Len, /* UTF16 */ true);
	}
	else
	{
		InOutIt = NameData + Len * sizeof(ANSICHAR);
		return FNameSerializedView(NameData, Len, /* UTF16 */ false);
	}
}

#if ALLOW_NAME_BATCH_SAVING

static FNameSerializedView SaveAnsiName(TArray<uint8>& Out, const ANSICHAR* Src, uint32 Len)
{
	ANSICHAR* Dst = AddUninitializedElements<ANSICHAR>(Out, Len);
	FMemory::Memcpy(Dst, Src, Len * sizeof(ANSICHAR));

	return FNameSerializedView(Dst, Len);
}

static FNameSerializedView SaveUtf16Name(TArray<uint8>& Out, const WIDECHAR* Src, uint32 Len)
{
	// Align to UTF16CHAR after header
	AlignTo<UTF16CHAR>(Out);
	
#if !PLATFORM_LITTLE_ENDIAN
	#error TODO: Implement saving code units as Little-Endian on Big-Endian platforms
#endif

	// This is a no-op when sizeof(UTF16CHAR) == sizeof(WIDECHAR), which it usually is
	FTCHARToUTF16 Utf16String(Src, Len);

	UTF16CHAR* Dst = AddUninitializedElements<UTF16CHAR>(Out, Utf16String.Length());
	FMemory::Memcpy(Dst, Utf16String.Get(), Utf16String.Length() * sizeof(UTF16CHAR));

	return FNameSerializedView(Dst, Len);
}

static FNameSerializedView SaveAnsiOrUtf16Name(TArray<uint8>& Out, FNameStringView Name)
{
	void* HeaderData = AddUninitializedBytes(Out, sizeof(FSerializedNameHeader));
	new (HeaderData) FSerializedNameHeader(Name.Len, Name.bIsWide);

	if (Name.bIsWide)
	{
		return SaveUtf16Name(Out, Name.Wide, Name.Len);
	}
	else
	{
		return SaveAnsiName(Out, Name.Ansi, Name.Len);
	}
}

void SaveNameBatch(TArrayView<const FNameEntryId> Names, TArray<uint8>& OutNameData, TArray<uint8>& OutHashData)
{
	OutNameData.Empty(/* average bytes per name guesstimate */ 40 * Names.Num());
	OutHashData.Empty((/* hash version */ 1 + Names.Num()) * sizeof(uint64));

	// Save hash algorithm version
	AddValue(OutHashData, INTEL_ORDER64(FNameHash::AlgorithmId));

	// Save names and hashes
	FNameBuffer CustomDecodeBuffer;
	for (FNameEntryId EntryId : Names)
	{
		FNameStringView InMemoryName = GetNamePoolPostInit().Resolve(EntryId).MakeView(CustomDecodeBuffer);
		FNameSerializedView SavedName = SaveAnsiOrUtf16Name(OutNameData, InMemoryName);

		uint64 LowerHash = SavedName.bIsUtf16 ? FNameHash::GenerateLowerCaseHash(SavedName.Utf16, SavedName.Len)
											  : FNameHash::GenerateLowerCaseHash(SavedName.Ansi,  SavedName.Len);

		AddValue(OutHashData, INTEL_ORDER64(LowerHash));
	}
}

#endif // WITH_EDITOR

FORCENOINLINE void ReserveNameBatch(uint32 NameDataBytes, uint32 HashDataBytes)
{
	uint32 NumEntries = HashDataBytes / sizeof(uint64) - 1;
	// Add 20% slack to reduce probing costs
	auto AddSlack = [](uint64 In){ return static_cast<uint32>(In * 6 / 5); };
	GetNamePoolPostInit().Reserve(AddSlack(NameDataBytes), AddSlack(NumEntries));
}

static FNameEntryId BatchLoadNameWithoutHash(const UTF16CHAR* Str, uint32 Len)
{
	WIDECHAR Temp[NAME_SIZE];
	for (uint32 Idx = 0; Idx < Len; ++Idx)
	{
		Temp[Idx] = INTEL_ORDER16(Str[Idx]);
	}
	
#if PLATFORM_TCHAR_IS_4_BYTES
	// Inline combine any surrogate pairs in the data when loading into a UTF-32 string
	Len = StringConv::InlineCombineSurrogates_Buffer(Temp, Len);
#endif

	FNameStringView Name(Temp, Len);
	FNameHash Hash = HashName<ENameCase::IgnoreCase>(Name);
	return GetNamePoolPostInit().BatchStore(FNameComparisonValue(Name, Hash));
}

static FNameEntryId BatchLoadNameWithoutHash(const ANSICHAR* Str, uint32 Len)
{
	FNameStringView Name(Str, Len);
	FNameHash Hash = HashName<ENameCase::IgnoreCase>(Name);
	return GetNamePoolPostInit().BatchStore(FNameComparisonValue(Name, Hash));
}

static FNameEntryId BatchLoadNameWithoutHash(const FNameSerializedView& Name)
{
	return Name.bIsUtf16 ? BatchLoadNameWithoutHash(Name.Utf16, Name.Len)
						 : BatchLoadNameWithoutHash(Name.Ansi, Name.Len);
}

template<typename CharType>
FNameEntryId BatchLoadNameWithHash(const CharType* Str, uint32 Len, uint64 InHash)
{
	FNameStringView Name(Str, Len);
	FNameHash Hash(Str, Len, InHash);
	checkfSlow(Hash == HashName<ENameCase::IgnoreCase>(Name), TEXT("Precalculated hash was wrong"));
	return GetNamePoolPostInit().BatchStore(FNameComparisonValue(Name, Hash));
}

static FNameEntryId BatchLoadNameWithHash(const FNameSerializedView& InName, uint64 InHash)
{
	if (InName.bIsUtf16)
	{
		// Wide names and hashes are currently stored as UTF16 Little-Endian 
		// regardless of target architecture. 
#if PLATFORM_LITTLE_ENDIAN
		if (sizeof(UTF16CHAR) == sizeof(WIDECHAR))
		{
			return BatchLoadNameWithHash(reinterpret_cast<const WIDECHAR*>(InName.Utf16), InName.Len, InHash);
		}
#endif

		return BatchLoadNameWithoutHash(InName.Utf16, InName.Len);
	}
	else
	{
		return BatchLoadNameWithHash(InName.Ansi, InName.Len, InHash);
	}
}

void LoadNameBatch(TArray<FNameEntryId>& OutNames, TArrayView<const uint8> NameData, TArrayView<const uint8> HashData)
{
	check(IsAligned(NameData.GetData(), sizeof(uint64)));
	check(IsAligned(HashData.GetData(), sizeof(uint64)));
	check(IsAligned(HashData.Num(), sizeof(uint64)));
	check(HashData.Num() > 0);

	const uint8* NameIt = NameData.GetData();
	const uint8* NameEnd = NameData.GetData() + NameData.Num();

	const uint64* HashDataIt = reinterpret_cast<const uint64*>(HashData.GetData());
	uint64 HashVersion = INTEL_ORDER64(HashDataIt[0]);
	TArrayView<const uint64> Hashes = MakeArrayView(HashDataIt + 1, HashData.Num() / sizeof(uint64) - 1);

	OutNames.Empty(Hashes.Num());

	GetNamePoolPostInit().BatchLock();

	if (HashVersion == FNameHash::AlgorithmId)
	{
		for (uint64 Hash : Hashes)
		{
			check(NameIt < NameEnd);
			FNameSerializedView Name = LoadNameHeader(/* in-out */ NameIt);
			OutNames.Add(BatchLoadNameWithHash(Name, INTEL_ORDER64(Hash)));
		}
	}
	else
	{
		while (NameIt < NameEnd)
		{
			FNameSerializedView Name = LoadNameHeader(/* in-out */ NameIt);
			OutNames.Add(BatchLoadNameWithoutHash(Name));
		}
	
	}

	GetNamePoolPostInit().BatchUnlock();

	check(NameIt == NameEnd);
}

#if 0 && ALLOW_NAME_BATCH_SAVING  

FORCENOINLINE void PerfTestLoadNameBatch(TArray<FNameEntryId>& OutNames, TArrayView<const uint8> NameData, TArrayView<const uint8> HashData)
{
	LoadNameBatch(OutNames, NameData, HashData);
}

static bool WithinBlock(uint8* BlockBegin, const FNameEntry* Entry)
{
	return UPTRINT(Entry) >= UPTRINT(BlockBegin) && UPTRINT(Entry) < UPTRINT(BlockBegin) + FNameEntryAllocator::BlockSizeBytes;
}

#include "GenericPlatform\GenericPlatformFile.h"

static void WriteBlobFile(const TCHAR* FileName, const TArray<uint8>& Blob)
{
	TUniquePtr<IFileHandle> FileHandle(IPlatformFile::GetPlatformPhysical().OpenWrite(FileName));
	FileHandle->Write(Blob.GetData(), Blob.Num());
}

static TArray<uint8> ReadBlobFile(const TCHAR* FileName)
{
	TArray<uint8> Out;
	TUniquePtr<IFileHandle> FileHandle(IPlatformFile::GetPlatformPhysical().OpenRead(FileName));
	if (FileHandle)
	{
		Out.AddUninitialized(FileHandle->Size());
		FileHandle->Read(Out.GetData(), Out.Num());
	}

	return Out;
}

CORE_API int SaveNameBatchTestFiles();
CORE_API int LoadNameBatchTestFiles();

int SaveNameBatchTestFiles()
{
	uint8** Blocks = GetNamePool().GetBlocksForDebugVisualizer();
	uint32 BlockIdx = 0;
	TArray<FNameEntryId> NameEntries;
	for (const FNameEntry* Entry : FName::DebugDump())
	{
		BlockIdx += !WithinBlock(Blocks[BlockIdx], Entry);
		check(WithinBlock(Blocks[BlockIdx], Entry));

		FNameEntryHandle Handle(BlockIdx, (UPTRINT(Entry) - UPTRINT(Blocks[BlockIdx]))/FNameEntryAllocator::Stride);
		NameEntries.Add(Handle);
	}

	TArray<uint8> NameData;
	TArray<uint8> HashData;
	SaveNameBatch(MakeArrayView(NameEntries), NameData, HashData);

	WriteBlobFile(TEXT("TestNameBatch.Names"), NameData);
	WriteBlobFile(TEXT("TestNameBatch.Hashes"), HashData);

	return NameEntries.Num();
}

int LoadNameBatchTestFiles()
{
	TArray<uint8> NameData = ReadBlobFile(TEXT("TestNameBatch.Names"));
	TArray<uint8> HashData = ReadBlobFile(TEXT("TestNameBatch.Hashes"));

	TArray<FNameEntryId> NameEntries;
	if (HashData.Num())
	{
		ReserveNameBatch(NameData.Num(), HashData.Num());
		PerfTestLoadNameBatch(NameEntries, MakeArrayView(NameData), MakeArrayView(HashData));
	}
	return NameEntries.Num();
}

#endif

static void TestNameBatch()
{
#if ALLOW_NAME_BATCH_SAVING

	TArray<FNameEntryId> Names;
	TArray<uint8> NameData;
	TArray<uint8> HashData;

	// Test empty batch
	SaveNameBatch(MakeArrayView(Names), NameData, HashData);
	check(NameData.Num() == 0);
	LoadNameBatch(Names, MakeArrayView(NameData), MakeArrayView(HashData));
	check(Names.Num() == 0);

	// Test empty / "None" name and another EName
	Names.Add(FName().GetComparisonIndex());
	Names.Add(FName(NAME_Box).GetComparisonIndex());

	// Test long strings
	FString MaxLengthAnsi;
	MaxLengthAnsi.Reserve(NAME_SIZE);
	while (MaxLengthAnsi.Len() < NAME_SIZE)
	{
		MaxLengthAnsi.Append("0123456789ABCDEF");
	}
	MaxLengthAnsi = MaxLengthAnsi.Left(NAME_SIZE - 1);

	FString MaxLengthWide = MaxLengthAnsi;
	MaxLengthWide[200] = 500;

	for (const FString& MaxLength : {MaxLengthAnsi, MaxLengthWide})
	{
		Names.Add(FName(*MaxLength).GetComparisonIndex());
		Names.Add(FName(*MaxLength + NAME_SIZE - 255).GetComparisonIndex());
		Names.Add(FName(*MaxLength + NAME_SIZE - 256).GetComparisonIndex());
		Names.Add(FName(*MaxLength + NAME_SIZE - 257).GetComparisonIndex());
	}

	// Test UTF-16 alignment
	FString Wide("Wide ");
	Wide[4] = 60000;

	Names.Add(FName(*Wide).GetComparisonIndex());
	Names.Add(FName("odd").GetComparisonIndex());
	Names.Add(FName(*Wide).GetComparisonIndex());
	Names.Add(FName("even").GetComparisonIndex());
	Names.Add(FName(*Wide).GetComparisonIndex());

	// Roundtrip names
	SaveNameBatch(MakeArrayView(Names), NameData, HashData);
	check(NameData.Num() > 0);
	TArray<FNameEntryId> LoadedNames;
	LoadNameBatch(LoadedNames, MakeArrayView(NameData), MakeArrayView(HashData));
	check(LoadedNames == Names);

	// Test changing hash version
	HashData[0] = 0xba;
	HashData[1] = 0xad;
	LoadNameBatch(LoadedNames, MakeArrayView(NameData), MakeArrayView(HashData));
	check(LoadedNames == Names);

	// Test determinism
	TArray<uint8> NameData2;
	TArray<uint8> HashData2;

	auto ClearAndReserveMemoryWithBytePattern = 
		[](TArray<uint8>& Out, uint8 Pattern, uint32 Num)
	{
		Out.Init(Pattern, Num);
		Out.Empty(Out.Max());
	};
	
	ClearAndReserveMemoryWithBytePattern(NameData2, 0xaa, NameData.Num());
	ClearAndReserveMemoryWithBytePattern(HashData2, 0xaa, HashData.Num());
	ClearAndReserveMemoryWithBytePattern(NameData,	0xbb, NameData.Num());
	ClearAndReserveMemoryWithBytePattern(HashData,	0xbb, HashData.Num());
	
	SaveNameBatch(MakeArrayView(Names), NameData, HashData);
	SaveNameBatch(MakeArrayView(Names), NameData2, HashData2);
	
	check(NameData == NameData2);
	check(HashData == HashData2);

#endif // ALLOW_NAME_BATCH_SAVING
}

#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST

#include "Containers/StackTracker.h"
static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn(
	TEXT("LogGameThreadFNameChurn.Enable"),
	0,
	TEXT("If > 0, then collect sample game thread fname create, periodically print a report of the worst offenders."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_PrintFrequency(
	TEXT("LogGameThreadFNameChurn.PrintFrequency"),
	300,
	TEXT("Number of frames between churn reports."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_Threshhold(
	TEXT("LogGameThreadFNameChurn.Threshhold"),
	10,
	TEXT("Minimum average number of fname creations per frame to include in the report."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_SampleFrequency(
	TEXT("LogGameThreadFNameChurn.SampleFrequency"),
	1,
	TEXT("Number of fname creates per sample. This is used to prevent churn sampling from slowing the game down too much."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_StackIgnore(
	TEXT("LogGameThreadFNameChurn.StackIgnore"),
	4,
	TEXT("Number of items to discard from the top of a stack frame."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_RemoveAliases(
	TEXT("LogGameThreadFNameChurn.RemoveAliases"),
	1,
	TEXT("If > 0 then remove aliases from the counting process. This essentialy merges addresses that have the same human readable string. It is slower."));

static TAutoConsoleVariable<int32> CVarLogGameThreadFNameChurn_StackLen(
	TEXT("LogGameThreadFNameChurn.StackLen"),
	3,
	TEXT("Maximum number of stack frame items to keep. This improves aggregation because calls that originate from multiple places but end up in the same place will be accounted together."));


struct FSampleFNameChurn
{
	FStackTracker GGameThreadFNameChurnTracker;
	bool bEnabled;
	int32 CountDown;
	uint64 DumpFrame;

	FSampleFNameChurn()
		: bEnabled(false)
		, CountDown(MAX_int32)
		, DumpFrame(0)
	{
	}

	void NameCreationHook()
	{
		bool bNewEnabled = CVarLogGameThreadFNameChurn.GetValueOnGameThread() > 0;
		if (bNewEnabled != bEnabled)
		{
			check(IsInGameThread());
			bEnabled = bNewEnabled;
			if (bEnabled)
			{
				CountDown = CVarLogGameThreadFNameChurn_SampleFrequency.GetValueOnGameThread();
				DumpFrame = GFrameCounter + CVarLogGameThreadFNameChurn_PrintFrequency.GetValueOnGameThread();
				GGameThreadFNameChurnTracker.ResetTracking();
				GGameThreadFNameChurnTracker.ToggleTracking(true, true);
			}
			else
			{
				GGameThreadFNameChurnTracker.ToggleTracking(false, true);
				DumpFrame = 0;
				GGameThreadFNameChurnTracker.ResetTracking();
			}
		}
		else if (bEnabled)
		{
			check(IsInGameThread());
			check(DumpFrame);
			if (--CountDown <= 0)
			{
				CountDown = CVarLogGameThreadFNameChurn_SampleFrequency.GetValueOnGameThread();
				CollectSample();
				if (GFrameCounter > DumpFrame)
				{
					PrintResultsAndReset();
				}
			}
		}
	}

	void CollectSample()
	{
		check(IsInGameThread());
		GGameThreadFNameChurnTracker.CaptureStackTrace(CVarLogGameThreadFNameChurn_StackIgnore.GetValueOnGameThread(), nullptr, CVarLogGameThreadFNameChurn_StackLen.GetValueOnGameThread(), CVarLogGameThreadFNameChurn_RemoveAliases.GetValueOnGameThread() > 0);
	}
	void PrintResultsAndReset()
	{
		DumpFrame = GFrameCounter + CVarLogGameThreadFNameChurn_PrintFrequency.GetValueOnGameThread();
		FOutputDeviceRedirector* Log = FOutputDeviceRedirector::Get();
		float SampleAndFrameCorrection = float(CVarLogGameThreadFNameChurn_SampleFrequency.GetValueOnGameThread()) / float(CVarLogGameThreadFNameChurn_PrintFrequency.GetValueOnGameThread());
		GGameThreadFNameChurnTracker.DumpStackTraces(CVarLogGameThreadFNameChurn_Threshhold.GetValueOnGameThread(), *Log, SampleAndFrameCorrection);
		GGameThreadFNameChurnTracker.ResetTracking();
	}
};

FSampleFNameChurn GGameThreadFNameChurnTracker;

void CallNameCreationHook()
{
	if (GIsRunning && IsInGameThread())
	{
		GGameThreadFNameChurnTracker.NameCreationHook();
	}
}

#endif

uint8** FNameDebugVisualizer::GetBlocks()
{
	static_assert(EntryStride == FNameEntryAllocator::Stride,	"Natvis constants out of sync with actual constants");
	static_assert(BlockBits == FNameMaxBlockBits,				"Natvis constants out of sync with actual constants");
	static_assert(OffsetBits == FNameBlockOffsetBits,			"Natvis constants out of sync with actual constants");

	return ((FNamePool*)(NamePoolData))->GetBlocksForDebugVisualizer();
}

FString FScriptName::ToString() const
{
	return ScriptNameToName(*this).ToString();
}

void Freeze::IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const FName& Object, const FTypeLayoutDesc&)
{
	Writer.WriteFName(Object);
}

uint32 Freeze::IntrinsicAppendHash(const FName* DummyObject, const FTypeLayoutDesc& TypeDesc, const FPlatformTypeLayoutParameters& LayoutParams, FSHA1& Hasher)
{
	const uint32 SizeFromFields = LayoutParams.WithCasePreservingFName() ? sizeof(FScriptName) : sizeof(FMinimalName);
	return Freeze::AppendHashForNameAndSize(TypeDesc.Name, SizeFromFields, Hasher);
}

void Freeze::IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const FMinimalName& Object, const FTypeLayoutDesc&)
{
	Writer.WriteFMinimalName(Object);
}

void Freeze::IntrinsicWriteMemoryImage(FMemoryImageWriter& Writer, const FScriptName& Object, const FTypeLayoutDesc&)
{
	Writer.WriteFScriptName(Object);
}

PRAGMA_ENABLE_UNSAFE_TYPECAST_WARNINGS
