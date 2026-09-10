//===- bolt/runtime/iwyn.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if (defined(__x86_64__) || defined(__aarch64__) || defined(__arm64__)) &&     \
    !defined(__APPLE__)

#if defined(__x86_64__) || defined(__aarch64__) || defined(__arm64__)
#include "common.h"
#endif

#if !defined(__x86_64__) && !defined(__aarch64__) && !defined(__arm64__)
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using int64_t = long long;
#endif

#pragma GCC visibility push(hidden)

extern "C" {
int64_t __bolt_iwyn_dynamic_offset;
uint64_t __bolt_iwyn_resolved_entry_hook;
uint64_t __bolt_iwyn_resolved_exit_hook;
}

namespace {

static const char EntryHookName[] = "__bolt_probe_enter";
static const char ExitHookName[] = "__bolt_probe_exit";

enum HookState : uint64_t {
  HookUnresolved = 0,
  HookResolving = 1,
  HookMissing = 2,
};

static bool isResolvedHook(uint64_t Address) { return Address > HookMissing; }

static_assert(HookMissing == 2,
              "dispatcher assembly expects Missing to be 2");

struct LinkMap {
  uint64_t Address;
  const char *Name;
  uint64_t *Dynamic;
  LinkMap *Next;
  LinkMap *Prev;
};

struct RDebug {
  int Version;
  LinkMap *Map;
};

struct ElfSymbol {
  uint32_t Name;
  uint8_t Info;
  uint8_t Other;
  uint16_t SectionIndex;
  uint64_t Value;
  uint64_t Size;
};

enum : uint64_t {
  DynNull = 0,
  DynPltGot = 3,
  DynHash = 4,
  DynStrTab = 5,
  DynSymTab = 6,
  DynSymEnt = 11,
  DynDebug = 21,
  DynGNUHash = 0x6ffffef5,
};

enum : uint8_t {
  SymbolBindingLocal = 0,
  SymbolVisibilityMask = 0x3,
  SymbolVisibilityHidden = 2,
  SymbolVisibilityInternal = 1,
};

enum : uint64_t {
  AuxNull = 0,
  AuxBase = 7,
};

enum : uint32_t {
  ProgramHeaderDynamic = 2,
};

struct AuxVectorEntry {
  uint64_t Type;
  uint64_t Value;
};

struct ElfHeader {
  uint8_t Ident[16];
  uint16_t Type;
  uint16_t Machine;
  uint32_t Version;
  uint64_t Entry;
  uint64_t ProgramHeaderOffset;
  uint64_t SectionHeaderOffset;
  uint32_t Flags;
  uint16_t HeaderSize;
  uint16_t ProgramHeaderEntrySize;
  uint16_t ProgramHeaderCount;
  uint16_t SectionHeaderEntrySize;
  uint16_t SectionHeaderCount;
  uint16_t SectionNameIndex;
};

struct ProgramHeader {
  uint32_t Type;
  uint32_t Flags;
  uint64_t Offset;
  uint64_t VirtualAddress;
  uint64_t PhysicalAddress;
  uint64_t FileSize;
  uint64_t MemorySize;
  uint64_t Alignment;
};

static uint64_t getLoaderBase() {
  static const char AuxvPath[] = "/proc/self/auxv";
  const int64_t FD = static_cast<int64_t>(__open(AuxvPath, O_RDONLY, 0));
  if (FD < 0)
    return 0;

  uint64_t Base = 0;
  AuxVectorEntry Entry;
  while (__read(static_cast<uint64_t>(FD), &Entry, sizeof(Entry)) ==
         sizeof(Entry)) {
    if (Entry.Type == AuxBase) {
      Base = Entry.Value;
      break;
    }
    if (Entry.Type == AuxNull)
      break;
  }
  __close(static_cast<uint64_t>(FD));
  return Base;
}

static uint32_t gnuHash(const char *Name) {
  uint32_t Hash = 5381;
  for (; *Name; ++Name)
    Hash = Hash * 33 + static_cast<uint8_t>(*Name);
  return Hash;
}

static uint32_t elfHash(const char *Name) {
  uint32_t Hash = 0;
  for (; *Name; ++Name) {
    Hash = (Hash << 4) + static_cast<uint8_t>(*Name);
    const uint32_t High = Hash & 0xf0000000;
    if (High)
      Hash ^= High >> 24;
    Hash &= ~High;
  }
  return Hash;
}

static bool equalString(const char *Left, const char *Right) {
  while (*Left && *Left == *Right) {
    ++Left;
    ++Right;
  }
  return *Left == *Right;
}

static uint64_t dynamicPointer(const LinkMap *Map, uint64_t Value) {
  if (Map->Address && Value < Map->Address)
    return Map->Address + Value;
  return Value;
}

static const ElfSymbol *lookupGNUHash(const uint32_t *Table,
                                      const ElfSymbol *Symbols,
                                      const char *Strings, const char *Name) {
  const uint32_t NumBuckets = Table[0];
  const uint32_t SymbolOffset = Table[1];
  const uint32_t BloomSize = Table[2];
  const uint32_t BloomShift = Table[3];
  if (!NumBuckets || !BloomSize)
    return nullptr;

  const uint64_t *Bloom = reinterpret_cast<const uint64_t *>(Table + 4);
  const uint32_t *Buckets =
      reinterpret_cast<const uint32_t *>(Bloom + BloomSize);
  const uint32_t *Chains = Buckets + NumBuckets;
  const uint32_t Hash = gnuHash(Name);
  const uint64_t Word = Bloom[(Hash / 64) % BloomSize];
  const uint64_t Mask = (uint64_t(1) << (Hash % 64)) |
                        (uint64_t(1) << ((Hash >> BloomShift) % 64));
  if ((Word & Mask) != Mask)
    return nullptr;

  uint32_t Index = Buckets[Hash % NumBuckets];
  if (Index < SymbolOffset)
    return nullptr;
  for (;;) {
    const uint32_t Chain = Chains[Index - SymbolOffset];
    if ((Chain | 1) == (Hash | 1) &&
        equalString(Strings + Symbols[Index].Name, Name))
      return &Symbols[Index];
    if (Chain & 1)
      return nullptr;
    ++Index;
  }
}

static const ElfSymbol *lookupELFHash(const uint32_t *Table,
                                      const ElfSymbol *Symbols,
                                      const char *Strings, const char *Name) {
  const uint32_t NumBuckets = Table[0];
  if (!NumBuckets)
    return nullptr;
  const uint32_t *Buckets = Table + 2;
  const uint32_t *Chains = Buckets + NumBuckets;
  for (uint32_t Index = Buckets[elfHash(Name) % NumBuckets]; Index;
       Index = Chains[Index])
    if (equalString(Strings + Symbols[Index].Name, Name))
      return &Symbols[Index];
  return nullptr;
}

static void *lookupInObject(LinkMap *Map, const char *Name) {
  const char *Strings = nullptr;
  const ElfSymbol *Symbols = nullptr;
  const uint32_t *GNUHash = nullptr;
  const uint32_t *ELFHash = nullptr;
  uint64_t SymbolSize = sizeof(ElfSymbol);

  for (uint64_t *Dyn = Map->Dynamic; Dyn && Dyn[0] != DynNull; Dyn += 2) {
    switch (Dyn[0]) {
    case DynStrTab:
      Strings = reinterpret_cast<const char *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynSymTab:
      Symbols =
          reinterpret_cast<const ElfSymbol *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynSymEnt:
      SymbolSize = Dyn[1];
      break;
    case DynGNUHash:
      GNUHash = reinterpret_cast<const uint32_t *>(dynamicPointer(Map, Dyn[1]));
      break;
    case DynHash:
      ELFHash = reinterpret_cast<const uint32_t *>(dynamicPointer(Map, Dyn[1]));
      break;
    }
  }

  if (!Strings || !Symbols || SymbolSize != sizeof(ElfSymbol))
    return nullptr;
  const ElfSymbol *Symbol =
      GNUHash ? lookupGNUHash(GNUHash, Symbols, Strings, Name) : nullptr;
  if (!Symbol && ELFHash)
    Symbol = lookupELFHash(ELFHash, Symbols, Strings, Name);
  const uint8_t Binding = Symbol ? Symbol->Info >> 4 : SymbolBindingLocal;
  const uint8_t Visibility =
      Symbol ? Symbol->Other & SymbolVisibilityMask : SymbolVisibilityHidden;
  if (!Symbol || !Symbol->SectionIndex || Binding == SymbolBindingLocal ||
      Visibility == SymbolVisibilityHidden ||
      Visibility == SymbolVisibilityInternal)
    return nullptr;
  return reinterpret_cast<void *>(Map->Address + Symbol->Value);
}

static RDebug *getLoaderDebug() {
  const uint64_t LoaderBase = getLoaderBase();
  if (!LoaderBase)
    return nullptr;

  const ElfHeader *Header = reinterpret_cast<const ElfHeader *>(LoaderBase);
  if (Header->Ident[0] != 0x7f || Header->Ident[1] != 'E' ||
      Header->Ident[2] != 'L' || Header->Ident[3] != 'F' ||
      Header->Ident[4] != 2 ||
      Header->ProgramHeaderEntrySize != sizeof(ProgramHeader))
    return nullptr;

  const uint8_t *ProgramHeaders = reinterpret_cast<const uint8_t *>(
      LoaderBase + Header->ProgramHeaderOffset);
  for (uint16_t I = 0; I < Header->ProgramHeaderCount; ++I) {
    const ProgramHeader *Program = reinterpret_cast<const ProgramHeader *>(
        ProgramHeaders + I * Header->ProgramHeaderEntrySize);
    if (Program->Type != ProgramHeaderDynamic)
      continue;

    LinkMap LoaderMap{
        LoaderBase, nullptr,
        reinterpret_cast<uint64_t *>(LoaderBase + Program->VirtualAddress),
        nullptr, nullptr};
    return reinterpret_cast<RDebug *>(lookupInObject(&LoaderMap, "_r_debug"));
  }
  return nullptr;
}

static void *resolveHook(const char *HookName) {
  uint64_t *Dynamic = reinterpret_cast<uint64_t *>(
      reinterpret_cast<uint64_t>(&__bolt_iwyn_dynamic_offset) +
      __bolt_iwyn_dynamic_offset);
  RDebug *Debug = nullptr;
  LinkMap *MapHead = nullptr;
  for (uint64_t *Dyn = Dynamic; Dyn[0] != DynNull; Dyn += 2)
    if (Dyn[0] == DynDebug) {
      Debug = reinterpret_cast<RDebug *>(Dyn[1]);
      break;
    } else if (Dyn[0] == DynPltGot) {
      uint64_t *PltGot = reinterpret_cast<uint64_t *>(Dyn[1]);
      if (PltGot)
        MapHead = reinterpret_cast<LinkMap *>(PltGot[1]);
    }

  if (!Debug)
    Debug = getLoaderDebug();
  if (Debug)
    MapHead = Debug->Map;
  while (MapHead && MapHead->Prev)
    MapHead = MapHead->Prev;
  if (!MapHead)
    return nullptr;

  for (LinkMap *Map = MapHead; Map; Map = Map->Next)
    if (void *Address = lookupInObject(Map, HookName))
      return Address;
  return nullptr;
}

static void *resolveAndCache(uint64_t *Slot, const char *HookName) {
  uint64_t Address = __atomic_load_n(Slot, __ATOMIC_ACQUIRE);
  // Already successfully resolved, return the cached address.
  if (isResolvedHook(Address))
    return reinterpret_cast<void *>(Address);
  // Already failed to find the hook, return nullptr.
  if (Address != HookUnresolved)
    return nullptr;

  uint64_t Expected = HookUnresolved;
  if (!__atomic_compare_exchange_n(Slot, &Expected, HookResolving,
                                   /*Weak=*/false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE))
    return isResolvedHook(Expected) ? reinterpret_cast<void *>(Expected)
                                    : nullptr;

  void *HookAddress = resolveHook(HookName);
  __atomic_store_n(Slot,
                   HookAddress ? reinterpret_cast<uint64_t>(HookAddress)
                               : HookMissing,
                   __ATOMIC_RELEASE);
  return HookAddress;
}

extern "C" void *__bolt_iwyn_resolve_entry() {
  return resolveAndCache(&__bolt_iwyn_resolved_entry_hook, EntryHookName);
}

extern "C" void *__bolt_iwyn_resolve_exit() {
  return resolveAndCache(&__bolt_iwyn_resolved_exit_hook, ExitHookName);
}

} // namespace

extern "C" __attribute((naked)) void __bolt_iwyn_entry_dispatch() {
  // clang-format off
#if defined(__x86_64__)
  __asm__ __volatile__("mov __bolt_iwyn_resolved_entry_hook(%%rip), %%r11\n"
                       "test %%r11, %%r11\n"
                       "jz 1f\n"
                       "cmp $2, %%r11\n"
                       "jbe 3f\n"
                       "jmp *%%r11\n"
                       "1:\n"
                       SAVE_ALL
                       "call __bolt_iwyn_resolve_entry\n"
                       "test %%rax, %%rax\n"
                       "jz 2f\n"
                       "mov %%rax, 40(%%rsp)\n"
                       RESTORE_ALL
                       "push %%r11\n"
                       "ret\n"
                       "2:\n"
                       RESTORE_ALL
                       "3:\n"
                       "ret\n"
                       :::);
#elif defined(__aarch64__) || defined(__arm64__)
  __asm__ __volatile__("adrp x16, __bolt_iwyn_resolved_entry_hook\n"
                       "ldr x16, [x16, #:lo12:__bolt_iwyn_resolved_entry_hook]\n"
                       "cbz x16, 1f\n"
                       "cmp x16, #2\n"
                       "b.ls 3f\n"
                       "br x16\n"
                       "1:\n"
                       SAVE_ALL
                       "bl __bolt_iwyn_resolve_entry\n"
                       "cbz x0, 2f\n"
                       "str x0, [sp, #112]\n"
                       RESTORE_ALL
                       "br x16\n"
                       "2:\n"
                       RESTORE_ALL
                       "3:\n"
                       "ret\n"
                       :::);
#endif
  // clang-format on
}

extern "C" __attribute((naked)) void __bolt_iwyn_exit_dispatch() {
  // clang-format off
#if defined(__x86_64__)
  __asm__ __volatile__("mov __bolt_iwyn_resolved_exit_hook(%%rip), %%r11\n"
                       "test %%r11, %%r11\n"
                       "jz 1f\n"
                       "cmp $2, %%r11\n"
                       "jbe 3f\n"
                       "jmp *%%r11\n"
                       "1:\n"
                       SAVE_ALL
                       "call __bolt_iwyn_resolve_exit\n"
                       "test %%rax, %%rax\n"
                       "jz 2f\n"
                       "mov %%rax, 40(%%rsp)\n"
                       RESTORE_ALL
                       "push %%r11\n"
                       "ret\n"
                       "2:\n"
                       RESTORE_ALL
                       "3:\n"
                       "ret\n"
                       :::);
#elif defined(__aarch64__) || defined(__arm64__)
  __asm__ __volatile__("adrp x16, __bolt_iwyn_resolved_exit_hook\n"
                       "ldr x16, [x16, #:lo12:__bolt_iwyn_resolved_exit_hook]\n"
                       "cbz x16, 1f\n"
                       "cmp x16, #2\n"
                       "b.ls 3f\n"
                       "br x16\n"
                       "1:\n"
                       SAVE_ALL
                       "bl __bolt_iwyn_resolve_exit\n"
                       "cbz x0, 2f\n"
                       "str x0, [sp, #112]\n"
                       RESTORE_ALL
                       "br x16\n"
                       "2:\n"
                       RESTORE_ALL
                       "3:\n"
                       "ret\n"
                       :::);
#endif
  // clang-format on
}

#pragma GCC visibility pop

#endif
