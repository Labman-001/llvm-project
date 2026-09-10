//===- bolt/Passes/InstrumentWhatUNeed.cpp --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/InstrumentWhatUNeed.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "bolt/RuntimeLibs/IWYNRuntimeLibrary.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"

using namespace llvm;

namespace llvm {
namespace bolt {

namespace {

BinaryBasicBlock::iterator
insertInstructions(BinaryBasicBlock &BB, BinaryBasicBlock::iterator Pos,
                   const InstructionListType &Instructions) {
  for (const MCInst &Inst : Instructions) {
    MCInst Copy = Inst;
    Pos = BB.insertInstruction(Pos, std::move(Copy));
    ++Pos;
  }
  return Pos;
}

BinaryBasicBlock::iterator getEntryInsertionPoint(BinaryContext &BC,
                                                  BinaryBasicBlock &BB) {
  auto Pos = BB.begin();
  while (Pos != BB.end() && BC.MIB->isPseudo(*Pos))
    ++Pos;

  if (Pos == BB.end())
    return Pos;

  if (BC.isX86() && BC.MIB->isTerminateBranch(*Pos))
    return std::next(Pos);

  if (BC.isAArch64() && (BC.MIB->isBTILandingPad(*Pos, BTIKind::C) ||
                         BC.MIB->isBTILandingPad(*Pos, BTIKind::J) ||
                         BC.MIB->isBTILandingPad(*Pos, BTIKind::JC)))
    return std::next(Pos);

  return Pos;
}

bool isFragmentFamilyTailCall(BinaryContext &BC, const BinaryFunction &Function,
                              const MCInst &Inst) {
  if (!BC.MIB->isTailCall(Inst))
    return false;

  const MCSymbol *TargetSymbol = BC.MIB->getTargetSymbol(Inst);
  if (!TargetSymbol)
    return false;

  const BinaryFunction *TargetFunction =
      BC.getFunctionForSymbol(TargetSymbol);
  return TargetFunction && BC.areRelatedFragments(&Function, TargetFunction);
}

} // namespace

Error InstrumentWhatUNeed::runOnFunctions(BinaryContext &BC) {
  if (!opts::isInstrumentWhatUNeed())
    return Error::success();

  if (!BC.isELF() || (!BC.isX86() && !BC.isAArch64()))
    return createFatalBOLTError(
        "BOLT-ERROR: function entry/exit instrumentation supports ELF x86-64 "
        "and AArch64 only\n");

  SmallVector<std::string> FunctionPatterns(opts::InstrumentFuncList.begin(),
                                            opts::InstrumentFuncList.end());
  if (!opts::InstrumentFuncListFile.empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
        MemoryBuffer::getFile(opts::InstrumentFuncListFile);
    if (!Buffer)
      return createFatalBOLTError(
          Twine("BOLT-ERROR: cannot read --instrument-func-list-file \"") +
          opts::InstrumentFuncListFile + "\": " + Buffer.getError().message() +
          "\n");
    SmallVector<StringRef> FilePatterns;
    Buffer.get()->getBuffer().split(FilePatterns, ',', /*MaxSplit=*/-1,
                                    /*KeepEmpty=*/false);
    for (StringRef Pattern : FilePatterns)
      if (!Pattern.trim().empty())
        FunctionPatterns.push_back(Pattern.trim().str());
  }

  std::vector<Regex> FunctionFilters;
  FunctionFilters.reserve(FunctionPatterns.size());
  for (const std::string &Pattern : FunctionPatterns) {
    FunctionFilters.emplace_back(Pattern);
    std::string RegexError;
    if (!FunctionFilters.back().isValid(RegexError))
      return createFatalBOLTError(
          Twine("BOLT-ERROR: invalid --instrument-func-list regex \"") +
          Pattern + "\": " + RegexError + "\n");
  }

  auto matchesFunctionFilter = [&](const BinaryFunction &Function) {
    if (FunctionFilters.empty())
      return true;
    return Function
        .forEachName([&](StringRef Name) {
          return llvm::any_of(FunctionFilters, [&](const Regex &Filter) {
            return Filter.match(Name);
          });
        })
        .has_value();
  };

  DenseSet<const BinaryFunction *> HookFunctions;
  DenseSet<const BinaryFunction *> EarlyRuntimeFunctions;
  MCSymbol *EntryCallTarget = nullptr;
  MCSymbol *ExitCallTarget = nullptr;
  const bool ExternalHook = IWYNRuntimeLibrary::needsRuntime(BC);

  auto addFunctionFamily = [](DenseSet<const BinaryFunction *> &Functions,
                              BinaryFunction *Root) {
    if (!Root)
      return;
    SmallVector<BinaryFunction *> Worklist{Root};
    while (!Worklist.empty()) {
      BinaryFunction *Function = Worklist.pop_back_val();
      if (!Functions.insert(Function).second)
        continue;
      llvm::append_range(Worklist, Function->getFragments());
      if (Function->isFragment())
        llvm::append_range(Worklist, *Function->getParentFragments());
    }
  };

  auto findHook = [&](StringRef Name) -> MCSymbol * {
    BinaryData *Data = BC.getBinaryDataByName(Name);
    if (!Data)
      return nullptr;
    BinaryFunction *Function = BC.getFunctionForSymbol(Data->getSymbol());
    if (!Function)
      return nullptr;
    addFunctionFamily(HookFunctions, Function);
    return Data->getSymbol();
  };

  if (opts::instrumentFunctionEntry())
    EntryCallTarget =
        findHook(IWYNRuntimeLibrary::EntryHookName);
  if (opts::instrumentFunctionExit())
    ExitCallTarget = findHook(IWYNRuntimeLibrary::ExitHookName);

  auto createLocalDispatch = [&](StringRef FunctionName,
                                 StringRef OffsetName) -> MCSymbol * {
    if (!BC.getRuntimeLibrary())
      return nullptr;
    BinaryFunction *Dispatch =
        BC.createInjectedBinaryFunction(FunctionName.str());
    BinaryBasicBlock *BB = Dispatch->addBasicBlock();
    MCSymbol *Slot = BC.Ctx->getOrCreateSymbol(OffsetName);
    InstructionListType Instructions =
        BC.MIB->createInstrumentedFunctionDispatch(Slot, BC.Ctx.get());
    BB->addInstructions(Instructions.begin(), Instructions.end());
    BB->setCFIState(0);
    Dispatch->updateState(BinaryFunction::State::CFG_Finalized);
    return Dispatch->getSymbol();
  };

  if (opts::instrumentFunctionEntry() && !EntryCallTarget)
    EntryCallTarget = createLocalDispatch(
        "__bolt_iwyn_local_entry_dispatch",
        IWYNRuntimeLibrary::EntryDispatchOffsetName);
  if (opts::instrumentFunctionExit() && !ExitCallTarget)
    ExitCallTarget = createLocalDispatch(
        "__bolt_iwyn_local_exit_dispatch",
        IWYNRuntimeLibrary::ExitDispatchOffsetName);
  if ((opts::instrumentFunctionEntry() && !EntryCallTarget) ||
      (opts::instrumentFunctionExit() && !ExitCallTarget)) {
    return createFatalBOLTError(
        "BOLT-ERROR: external function entry/exit hook requires the "
        "resolver runtime\n");
  }

  if (ExternalHook) {
    auto addFunctionAtAddress = [&](uint64_t Address) {
      BinaryFunction *Function = BC.getBinaryFunctionAtAddress(Address);
      if (!Function)
        Function = BC.getBinaryFunctionContainingAddress(Address);
      addFunctionFamily(EarlyRuntimeFunctions, Function);
    };

    if (BC.InitAddress)
      addFunctionAtAddress(*BC.InitAddress);
    if (BC.FiniAddress)
      addFunctionAtAddress(*BC.FiniAddress);

    auto addArrayFunctions = [&](std::optional<uint64_t> ArrayAddress,
                                 std::optional<uint64_t> ArraySize) {
      if (!ArrayAddress || !ArraySize)
        return;
      ErrorOr<BinarySection &> Section = BC.getSectionForAddress(*ArrayAddress);
      if (!Section)
        return;

      const uint64_t PointerSize = BC.AsmInfo->getCodePointerSize();
      const uint64_t ArrayOffset = *ArrayAddress - Section->getAddress();
      for (uint64_t Offset = 0; Offset + PointerSize <= *ArraySize;
           Offset += PointerSize) {
        const uint64_t SectionOffset = ArrayOffset + Offset;
        const Relocation *Reloc =
            Section->getDynamicRelocationAt(SectionOffset);
        uint64_t TargetAddress = 0;
        if (Reloc) {
          if (Reloc->isRelative()) {
            TargetAddress = Reloc->Addend;
          } else if (Reloc->Symbol) {
            if (BinaryFunction *Function =
                    BC.getFunctionForSymbol(Reloc->Symbol)) {
              TargetAddress = Function->getAddress() + Reloc->Addend;
            }
          }
        } else if ((Reloc = Section->getRelocationAt(SectionOffset))) {
          TargetAddress = Reloc->Value;
        } else if (ErrorOr<uint64_t> Value =
                       BC.getPointerAtAddress(*ArrayAddress + Offset)) {
          TargetAddress = *Value;
        }
        if (TargetAddress)
          addFunctionAtAddress(TargetAddress);
      }
    };

    addArrayFunctions(BC.InitArrayAddress, BC.InitArraySize);
    addArrayFunctions(BC.FiniArrayAddress, BC.FiniArraySize);
  }

  uint64_t InstrumentedFunctions = 0;
  uint64_t EntryCalls = 0;
  uint64_t ExitCalls = 0;
  uint64_t SkippedFunctions = 0;
  DenseSet<const BinaryFunction *> PrintedFunctions;

  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    if (HookFunctions.contains(&Function) || Function.isPseudo() ||
        Function.isIgnored() || Function.isFolded() ||
        EarlyRuntimeFunctions.contains(&Function))
      continue;

    const BinaryFunction *CanonicalFunction = &Function;
    if (Function.isFragment() && Function.getParentFragments()->size() == 1)
      CanonicalFunction = *Function.getParentFragments()->begin();
    if (!matchesFunctionFilter(*CanonicalFunction))
      continue;

    if (!Function.hasCFG() || (!BC.HasRelocations && !Function.isSimple())) {
      ++SkippedFunctions;
      BC.errs() << "BOLT-WARNING: cannot move " << Function
                << " without input relocations; skipping "
                    "function instrumentation\n";
      continue;
    }

    InstructionListType EntryCall;
    InstructionListType ExitCall;
    if (opts::instrumentFunctionEntry())
      EntryCall = BC.MIB->createInstrumentedFunctionCall(
          EntryCallTarget, CanonicalFunction->getSymbol(), BC.Ctx.get());
    if (opts::instrumentFunctionExit())
      ExitCall = BC.MIB->createInstrumentedFunctionCall(
          ExitCallTarget, CanonicalFunction->getSymbol(), BC.Ctx.get());

    uint64_t FunctionEntries = 0;
    uint64_t FunctionExits = 0;
    for (BinaryBasicBlock &BB : Function) {
      const bool IsProcessEntry =
          ExternalHook && BC.StartFunctionAddress &&
          Function.getAddress() == *BC.StartFunctionAddress;
      if (opts::instrumentFunctionEntry() && !Function.isFragment() &&
          !IsProcessEntry && BB.isEntryPoint()) {
        insertInstructions(BB, getEntryInsertionPoint(BC, BB), EntryCall);
        ++FunctionEntries;
      }

      if (!opts::instrumentFunctionExit())
        continue;
      for (auto II = BB.begin(); II != BB.end(); ++II) {
        if (!BC.MIB->isReturn(*II) && !BC.MIB->isTailCall(*II))
          continue;
        if (isFragmentFamilyTailCall(BC, Function, *II))
          continue;

        II = insertInstructions(BB, II, ExitCall);
        ++FunctionExits;
      }
    }

    if (!FunctionEntries && !FunctionExits)
      continue;

    Function.setNeedsPatch(true);
    if (!BC.HasRelocations) {
      // Instrumentation usually makes the function too large for its original
      // space. Force it into newly allocated text; otherwise
      // CheckLargeFunctions may mark it non-simple and prevent its emission.
      Function.setMoveToNewAddress();
    }
    ++InstrumentedFunctions;
    EntryCalls += FunctionEntries;
    ExitCalls += FunctionExits;
    if (opts::InstrumentFuncPrint &&
        PrintedFunctions.insert(CanonicalFunction).second)
      BC.outs() << "BOLT-INFO: instrumented function "
                << CanonicalFunction->getPrintName() << '\n';
  }

  BC.outs() << "BOLT-INFO: function instrumentation inserted " << EntryCalls
            << " entry call(s) and " << ExitCalls << " exit call(s) in "
            << InstrumentedFunctions << " function(s)";
  if (SkippedFunctions)
    BC.outs() << "; skipped " << SkippedFunctions << " unsupported function(s)";
  BC.outs() << '\n';

  return Error::success();
}

} // namespace bolt
} // namespace llvm
