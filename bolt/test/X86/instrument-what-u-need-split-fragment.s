# Check function instrumentation for GCC-style hot/cold split functions in
# non-relocation mode. The cold fragment is not a separate function invocation:
# it receives exit probes but no extra entry probe.

# REQUIRES: system-linux,bolt-runtime

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux %t/lib.s -o %t.o
# RUN: ld.lld -shared -q %t.o -o %t.so
# RUN: cc %t/main.c -ldl -o %t.main
# RUN: cc -fPIC -shared %t/hook.c -o %t.hook.so
# RUN: llvm-bolt %t.so -o %t.bolt.so --relocs=0 --lite=0 \
# RUN:   --instrument=func-entry,func-exit --instrument-func-list='^foo' \
# RUN:   --instrument-func-print -v=1 2>&1 | FileCheck %s --check-prefix=BOLT
# RUN: llvm-objdump -d --disassemble-symbols=foo.org.0,foo.cold.1.org.0 \
# RUN:   %t.bolt.so | FileCheck %s --check-prefix=PATCH
# RUN: env LD_PRELOAD=%t.hook.so %t.main %t.bolt.so | \
# RUN:   FileCheck %s --check-prefix=EXEC

# BOLT: BOLT-INFO: marking foo.cold.1
# BOLT-SAME: as a fragment of foo
# BOLT-NOT: could not disassemble function foo
# BOLT: BOLT-INFO: instrumented function foo
# BOLT: BOLT-INFO: function instrumentation inserted 1 entry call(s) and 2 exit call(s) in 2 function(s)

# PATCH-LABEL: <foo.org.0>:
# PATCH-NEXT: {{.*}} jmp {{.*}} <foo>
# PATCH-LABEL: <foo.cold.1.org.0>:
# PATCH-NEXT: {{.*}} jmp {{.*}} <foo.cold.1>

# EXEC: entry-calls=2 exit-calls=2

//--- lib.s
.text
.globl foo
.type foo, @function
foo:
  .cfi_startproc
  testl %edi, %edi
  js foo.cold.1
  movl $7, %eax
  retq
  .cfi_endproc
.size foo, .-foo

.section .text.cold,"ax",@progbits
.local foo.cold.1
.type foo.cold.1, @function
foo.cold.1:
  .cfi_startproc
  movl $11, %eax
  retq
  .cfi_endproc
.size foo.cold.1, .-foo.cold.1

//--- main.c
#include <dlfcn.h>

typedef int (*foo_t)(int);

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  void *handle = dlopen(argv[1], RTLD_NOW);
  if (!handle)
    return 2;
  foo_t foo = (foo_t)dlsym(handle, "foo");
  if (!foo)
    return 3;
  int result = foo(1) + foo(-1);
  dlclose(handle);
  return result == 18 ? 0 : 4;
}

//--- hook.c
#include <stdint.h>
#include <stdio.h>

static unsigned entry_calls;
static unsigned exit_calls;

__attribute__((visibility("default"))) void
__bolt_probe_enter(uintptr_t function_pc) {
  if (function_pc)
    ++entry_calls;
}

__attribute__((visibility("default"))) void
__bolt_probe_exit(uintptr_t function_pc) {
  if (function_pc)
    ++exit_calls;
}

__attribute__((destructor)) static void report(void) {
  printf("entry-calls=%u exit-calls=%u\n", entry_calls, exit_calls);
}
