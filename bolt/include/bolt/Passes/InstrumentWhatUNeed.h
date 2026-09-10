//===- bolt/Passes/InstrumentWhatUNeed.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_INSTRUMENT_WHAT_U_NEED_H
#define BOLT_PASSES_INSTRUMENT_WHAT_U_NEED_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class InstrumentWhatUNeed : public BinaryFunctionPass {
public:
  explicit InstrumentWhatUNeed(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "instrument-what-u-need"; }
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
