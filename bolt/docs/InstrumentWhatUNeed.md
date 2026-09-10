# Function Entry/Exit Instrumentation
`--instrument` accepts one or more comma-separated modes: `func-entry` inserts
probes at function entries, while `func-exit` inserts probes before normal
returns and tail calls.

```sh
llvm-bolt input -o output --instrument=func-entry,func-exit
```

This mode supports x86-64 and AArch64 ELF executables, PIEs, and shared
libraries, with or without input relocations.

## Transformation

For a function with one entry and two normal exits, the transformation will
be like this:

```cpp
// Before
int classify(int value) {
  if (value < 0)
    return -1;
  return value + 1;
}

// After
int classify(int value) {
  __bolt_probe_enter(classify_pc);
  if (value < 0) {
    __bolt_probe_exit(classify_pc);
    return -1;
  }
  int result = value + 1;
  __bolt_probe_exit(classify_pc);
  return result;
}
```

Here `classify_pc` denotes the canonical runtime address of `classify`. The
pseudocode shows the semantic calls; each call is surrounded by generated
state-save and state-restore instructions. If the hook is defined in the input,
the generated sequence calls it directly. Otherwise it calls the corresponding
local dispatcher described below. BOLT inserts one entry probe in each
recognized function entry block and an exit probe before every normal return or
tail call. Both exits receive the same canonical function address.

Use `--instrument-func-list='regex1,regex2'` to instrument only functions whose
name or alias matches a POSIX extended regular expression. Use
`--instrument-func-list-file=<path>` to load comma-separated expressions from
a file; command-line and file expressions are combined. Without either option,
all supported functions are selected. `--instrument-func-print` prints each
successfully instrumented canonical function name.

## Hook ABI

Hooks use the platform C ABI and must have these exact unmangled names:

```cpp
extern "C" void __bolt_probe_enter(uint64_t function_pc);
extern "C" void __bolt_probe_exit(uint64_t function_pc);
```

`function_pc` is the canonical function's runtime entry address, including the
load bias of a PIE or shared object. Hooks must return normally and must not
throw exceptions. BOLT preserves caller-visible integer, flags, floating-point,
and SIMD state needed to call a normal C function at an arbitrary entry or exit.

## Hook Resolution

If a requested hook is defined in the input ELF, probe sites call it directly.
Otherwise BOLT links a resolver runtime, which requires a dynamically linked
ELF executable, PIE, or shared library. External hooks use this dispatch chain:

```text
probe site
  -> __bolt_iwyn_local_{entry,exit}_dispatch
  -> __bolt_iwyn_{entry,exit}_dispatch
  -> __bolt_probe_{enter,exit}
```

The local dispatcher loads a signed relative offset from the corresponding
`__bolt_iwyn_{entry,exit}_dispatch_offset` slot, adds it to the slot address,
and tail-jumps to the runtime dispatcher. After final addresses are known,
BOLT fills each slot with `runtime_dispatcher - slot`; this keeps the local
dispatcher position independent.

The runtime dispatcher first checks its cached hook address. On the first miss,
it preserves the probe arguments and registers, searches the dynamic loader's
loaded objects, and caches a successful result. It then restores the state and
tail-jumps to the hook, so the hook returns directly to the original probe
call site. Hooks may therefore be supplied with:

```sh
LD_PRELOAD=./libhooks.so ./output
```

If no loaded object exports a requested hook, that probe silently becomes a
no-op and execution continues. Both successful and failed lookups are cached,
so each requested hook is searched at most once. A hook loaded later with
`dlopen` is therefore not discovered after an earlier lookup failed.
