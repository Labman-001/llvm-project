// Check function entry and exit instrumentation of an x86-64 shared library.

// REQUIRES: system-linux,bolt-runtime

// RUN: split-file %s %t
// RUN: %clang %cflags -O2 -fno-inline -fPIC -c %t/lib.c -o %t/lib.o
// RUN: %clang -fPIC -shared %t/lib.o -Wl,-soname,libtarget.so -o %t/lib.so
// RUN: %clang %cflags -fPIC -c %t/hook.c -o %t/hook.o
// RUN: %clang -fPIC -shared %t/hook.o -o %t/hook.so
// RUN: llvm-bolt %t/lib.so -o %t/libtarget.so --relocs=0 \
// RUN:   --instrument=func-entry,func-exit --instrument-func-list='^target$'
// RUN: llvm-objdump -t %t/libtarget.so | FileCheck %s --check-prefix=LIB-SYMS
// RUN: %clang %cflags -c %t/main.c -o %t/main.o
// RUN: %clang %t/main.o -L%t -ltarget -o %t/main
// RUN: llvm-bolt %t/main -o %t/main.bolt --relocs=0 \
// RUN:   --instrument=func-entry,func-exit --instrument-func-list='^main$'
// RUN: llvm-objdump -t %t/main.bolt | FileCheck %s --check-prefix=MAIN-SYMS
// RUN: env LD_LIBRARY_PATH=%t LD_PRELOAD=%t/hook.so %t/main.bolt | FileCheck %s

// The instrumented body keeps the original name in .bolt.text. The .org.0
// symbol names the entry patch left at the original address in .text.
// LIB-SYMS-DAG: l F .text {{.*}} target.org.0
// LIB-SYMS-DAG: g F .bolt.text {{.*}} target
// MAIN-SYMS-DAG: l F .text {{.*}} main.org.0
// MAIN-SYMS-DAG: g F .bolt.text {{.*}} main

// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: enter pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}
// CHECK: exit pc=0x{{[1-9a-f][0-9a-f]*}}

//--- lib.c
__attribute__((noinline, visibility("default"))) int target(int value) {
  __asm__ volatile(".rept 16\nnop\n.endr");
  return value + 1;
}

//--- main.c
extern int target(int value);

int main(void) { return target(41) == 42 ? 0 : 1; }

//--- hook.c
#include <stdint.h>
#include <stdio.h>

void __bolt_probe_enter(uintptr_t function_pc) {
  printf("enter pc=0x%lx\n", (unsigned long)function_pc);
}

void __bolt_probe_exit(uintptr_t function_pc) {
  printf("exit pc=0x%lx\n", (unsigned long)function_pc);
}
