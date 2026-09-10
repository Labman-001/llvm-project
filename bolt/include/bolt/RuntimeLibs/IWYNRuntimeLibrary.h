//===- IWYNRuntimeLibrary.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_RUNTIMELIBS_IWYN_RUNTIME_LIBRARY_H
#define BOLT_RUNTIMELIBS_IWYN_RUNTIME_LIBRARY_H

#include "bolt/RuntimeLibs/RuntimeLibrary.h"

namespace llvm {
namespace bolt {

class IWYNRuntimeLibrary : public RuntimeLibrary {
public:
  static constexpr StringLiteral EntryHookName = "__bolt_probe_enter";
  static constexpr StringLiteral ExitHookName = "__bolt_probe_exit";
  static constexpr StringLiteral EntryDispatchName =
      "__bolt_iwyn_entry_dispatch";
  static constexpr StringLiteral ExitDispatchName = "__bolt_iwyn_exit_dispatch";
  static constexpr StringLiteral EntryDispatchOffsetName =
      "__bolt_iwyn_entry_dispatch_offset";
  static constexpr StringLiteral ExitDispatchOffsetName =
      "__bolt_iwyn_exit_dispatch_offset";
  static constexpr StringLiteral DynamicOffsetName =
      "__bolt_iwyn_dynamic_offset";

  void addRuntimeLibSections(std::vector<std::string> &SecNames) const final {
    SecNames.push_back(".bolt.iwyn");
  }

  void adjustCommandLineOptions(const BinaryContext &BC) const final;
  void emitBinary(BinaryContext &BC, MCStreamer &Streamer) final;
  void link(BinaryContext &BC, StringRef ToolPath, BOLTLinker &Linker,
            BOLTLinker::SectionsMapper MapSections) final;

  static bool needsRuntime(const BinaryContext &BC);
};

} // namespace bolt
} // namespace llvm

#endif
