//===- IWYNRuntimeLibrary.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/RuntimeLibs/IWYNRuntimeLibrary.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Core/Linker.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace bolt;

namespace opts {
cl::opt<std::string> RuntimeIWYNLib(
    "runtime-instrument-what-u-need-lib",
    cl::desc("specify path of the function probe resolver library"),
    cl::init("libbolt_rt_iwyn.a"), cl::cat(BoltInstrCategory));
} // namespace opts

bool IWYNRuntimeLibrary::needsRuntime(const BinaryContext &BC) {
  auto HasFunction = [&](StringRef Name) {
    const BinaryData *Data = BC.getBinaryDataByName(Name);
    return Data && BC.getFunctionForSymbol(Data->getSymbol());
  };
  return (opts::instrumentFunctionEntry() && !HasFunction(EntryHookName)) ||
         (opts::instrumentFunctionExit() && !HasFunction(ExitHookName));
}

void IWYNRuntimeLibrary::adjustCommandLineOptions(
    const BinaryContext &BC) const {
  assert(opts::isInstrumentWhatUNeed() &&
         "IWYN runtime requires function instrumentation");
  if (!BC.isELF() || BC.IsStaticExecutable) {
    errs() << "BOLT-ERROR: function entry/exit instrumentation with an "
              "external hook requires a dynamically linked ELF executable "
              "or shared library\n";
    exit(1);
  }
}

void IWYNRuntimeLibrary::emitBinary(BinaryContext &BC, MCStreamer &Streamer) {
  assert(opts::isInstrumentWhatUNeed() &&
         "IWYN runtime requires function instrumentation");
  MCSection *Section = BC.Ctx->getELFSection(
      ".bolt.iwyn", ELF::SHT_PROGBITS,
      BinarySection::getFlags(/*IsReadOnly=*/false, /*IsText=*/false,
                              /*IsAllocatable=*/true));
  Section->setAlignment(Align(8));
  Streamer.switchSection(Section);

  auto EmitGlobalLabel = [&](StringRef Name) {
    MCSymbol *Symbol = BC.Ctx->getOrCreateSymbol(Name);
    Streamer.emitSymbolAttribute(Symbol, MCSymbolAttr::MCSA_Global);
    Streamer.emitLabel(Symbol);
  };

  EmitGlobalLabel(EntryDispatchOffsetName);
  Streamer.emitIntValue(0, 8);
  EmitGlobalLabel(ExitDispatchOffsetName);
  Streamer.emitIntValue(0, 8);
}

void IWYNRuntimeLibrary::link(BinaryContext &BC, StringRef ToolPath,
                              BOLTLinker &Linker,
                              BOLTLinker::SectionsMapper MapSections) {
  assert(opts::isInstrumentWhatUNeed() &&
         "IWYN runtime requires function instrumentation");
  std::string LibPath = getLibPath(ToolPath, opts::RuntimeIWYNLib);
  loadLibrary(LibPath, Linker, MapSections);

  const auto EntryDispatch = Linker.lookupSymbolInfo(EntryDispatchName);
  const auto ExitDispatch = Linker.lookupSymbolInfo(ExitDispatchName);
  const auto EntryDispatchOffset =
      Linker.lookupSymbolInfo(EntryDispatchOffsetName);
  const auto ExitDispatchOffset =
      Linker.lookupSymbolInfo(ExitDispatchOffsetName);
  const auto DynamicOffset = Linker.lookupSymbolInfo(DynamicOffsetName);
  ErrorOr<BinarySection &> DynamicSection =
      BC.getUniqueSectionByName(".dynamic");
  if (!EntryDispatch || !ExitDispatch || !EntryDispatchOffset ||
      !ExitDispatchOffset || !DynamicOffset || !DynamicSection) {
    errs() << "BOLT-ERROR: cannot initialize function probe runtime\n";
    exit(1);
  }

  auto WriteRelativeOffset = [&](uint64_t SlotAddress, uint64_t TargetAddress) {
    for (BinarySection &Section : BC.allocatableSections()) {
      const uint64_t Start = Section.getOutputAddress();
      if (!Section.isFinalized() || !Section.getOutputData() ||
          !(Start <= SlotAddress &&
            SlotAddress + 8 <= Start + Section.getOutputSize()))
        continue;
      support::endian::write64le(Section.getOutputData() + SlotAddress - Start,
                                 TargetAddress - SlotAddress);
      return true;
    }
    return false;
  };

  if (!WriteRelativeOffset(EntryDispatchOffset->Address,
                           EntryDispatch->Address) ||
      !WriteRelativeOffset(ExitDispatchOffset->Address,
                           ExitDispatch->Address) ||
      !WriteRelativeOffset(DynamicOffset->Address,
                           DynamicSection->getAddress())) {
    errs() << "BOLT-ERROR: cannot locate function probe runtime slot\n";
    exit(1);
  }
}
