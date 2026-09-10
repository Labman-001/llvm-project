// Check function entry and exit instrumentation of an AArch64 shared library.

// REQUIRES: system-linux

// RUN: split-file %s %t
// RUN: %clang %cflags -target aarch64-linux -O2 -fno-inline -fPIC \
// RUN:   -c %t/lib.c -o %t/lib.o
// RUN: %clang %cflags -target aarch64-linux -shared -Wl,-q \
// RUN:   -Wl,-soname,libtarget.so %t/lib.o -o %t/lib.so
// RUN: llvm-bolt %t/lib.so -o %t/libtarget.so --relocs=0 \
// RUN:   --instrument=func-entry,func-exit --instrument-func-list='^target$' \
// RUN:   --lite=0 2>&1 | FileCheck %s --check-prefix=LIB-BOLT
// RUN: llvm-objdump -t %t/libtarget.so | FileCheck %s --check-prefix=LIB-SYMS
// RUN: %clang %cflags -target aarch64-linux -O2 -fno-inline -fPIC \
// RUN:   -c %t/main.c -o %t/main.o
// RUN: %clang %cflags -target aarch64-linux -pie -Wl,-q -Wl,-e,main \
// RUN:   %t/main.o -L%t -ltarget -o %t/main
// RUN: llvm-bolt %t/main -o %t/main.bolt --relocs=0 \
// RUN:   --instrument=func-entry,func-exit --instrument-func-list='^main$' \
// RUN:   --lite=0 2>&1 | FileCheck %s --check-prefix=MAIN-BOLT
// RUN: llvm-objdump -t %t/main.bolt | FileCheck %s --check-prefix=MAIN-SYMS

// LIB-BOLT: function instrumentation inserted 1 entry call(s) and 1 exit call(s) in 1 function(s)
// MAIN-BOLT: function instrumentation inserted 1 entry call(s) and 1 exit call(s) in 1 function(s)

// The instrumented body keeps the original name in .bolt.text. The .org.0
// symbol names the entry patch left at the original address in .text.
// LIB-SYMS-DAG: l F .text {{.*}} target.org.0
// LIB-SYMS-DAG: g F .bolt.text {{.*}} target
// MAIN-SYMS-DAG: l F .text {{.*}} main.org.0
// MAIN-SYMS-DAG: g F .bolt.text {{.*}} main

//--- hooks.h
void __bolt_probe_enter(unsigned long function_pc) {
  (void)function_pc;
}

void __bolt_probe_exit(unsigned long function_pc) {
  (void)function_pc;
}

//--- lib.c
#include "hooks.h"

__attribute__((noinline, visibility("default"))) int target(int value) {
  __asm__ volatile(".rept 16\nnop\n.endr");
  return value + 1;
}

//--- main.c
#include "hooks.h"

extern int target(int value);

int main(void) {
  __asm__ volatile(".rept 16\nnop\n.endr");
  return target(41) == 42 ? 0 : 1;
}
