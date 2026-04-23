# lona-mvm

`lona-mvm` is the managed runtime line for the `lona` language.

This repository starts from one architectural decision:

- `managed` is a target mode
- `bundle` and `run` are delivery modes inside that target

That distinction matters. Upstream `lona` docs already define `native` and
`managed` as two different compilation contracts. The MVM should not introduce
another fake target split just because programs may be launched in two ways.

## Runtime Goal

The managed virtual machine exists to execute `lona` managed artifacts through
JIT, while gradually adding runtime-managed features such as:

- GC
- reflection
- richer diagnostics
- better backtrace support

## Two Launch Profiles

The current design keeps one runtime core and exposes two startup profiles:

1. `run`
   - `mvm` loads a managed payload passed on the command line
   - good for development, testing, and tooling
2. `bundle`
   - the managed payload is embedded into an executable image
   - good for shipping a single file without requiring a separate `mvm` command

Both profiles should reuse the same loader, ABI contract, and JIT pipeline.

## Runtime Topology

The intended runtime shape is:

- main thread
  - CLI, module preparation, and shutdown
- mutator thread
  - runs JITed managed code
- VM/runtime thread
  - owns global runtime coordination
  - maintains code/source metadata
  - becomes the future GC and safepoint coordinator

This VM thread is for coordination, not for magical crash containment.

## Canonical Payload Format

Although the language frontend can print textual LLVM IR, the MVM should treat
LLVM bitcode as the canonical deployable payload.

Reasons:

- upstream `lona` already uses bitcode/object artifacts as the real reusable
  compiler outputs
- bitcode is smaller and faster to load than textual `.ll`
- bitcode is a better container for later managed metadata growth
- text IR can remain a debug input, but should not be the main packaging format

## Documents

- [Architecture](docs/architecture.md)

## Immediate Next Steps

- define the managed payload format and loader boundary
- build a minimal `mvm run <payload>` path that can JIT an entry symbol
- add bundled executable support on top of the same runtime core
- delay GC and reflection until the runtime ABI boundary is stable

## Current Prototype

The repository now contains a minimal `mvm` binary with this execution flow:

- load one LLVM bitcode module
- run LLVM IR verification
- run a managed verifier layer for MVM-specific legality checks
- run an LLVM optimization pipeline
- re-verify
- JIT the module with ORC `LLJIT`
- resolve and invoke an entry symbol

The prototype now also exports an initial managed memory ABI for future managed
lowering work.

The managed entry call itself now runs on a dedicated mutator thread. The main
thread remains the CLI/bootstrap path, and a separate VM/runtime thread stays
alive as the future home for safepoint and GC coordination.

The pipeline now also prepares managed functions for LLVM's `statepoint`
infrastructure by assigning `gc "statepoint-example"`, injecting a
`gc.safepoint_poll` helper, and running safepoint/statepoint lowering passes.
After that rewrite step, MVM also emits a first GC metadata summary layer:
`!mvm.gc.module`, `!mvm.gc.function`, `!mvm.gc.statepoint`, and
`!mvm.gc.relocate`. This is the current bridge between "LLVM has already placed
safepoints" and "the runtime can start reasoning about concrete GC points and
root relocation sites".

Before that GC lowering step, MVM now also runs a first managed-state analysis
pass. It seeds provenance from `__mvm_malloc` and `__mvm_array_malloc`,
propagates that state through local pointer SSA and direct calls, and annotates
the resulting function signatures and pointer-producing instructions with
`!mvm.managed.*` metadata. This is the current bridge between "compiler only
enforces pointer restrictions" and future address-space based managed-pointer
rewriting.

Managed execution now treats `O1` as the minimum supported optimization level.
`mvm` rejects `-O0` explicitly, and the managed GC/root-scan path is exercised
only on `O1+` code shapes.

Current default entry lookup order:

- `__mvm_main__`
- `__lona_main__`
- `main`

Supported entry signatures:

- `i32 ()`
- `void ()`
- `i32 (i32, ptr)`
- `void (i32, ptr)`

For `linked-bc` payloads that enter through `__lona_main__`, MVM now provides
the hosted argument bridge itself by exporting `@__lona_argc` and
`@__lona_argv`, setting them from the CLI arguments, and then invoking
`__lona_main__` directly. This avoids depending on a compiler-emitted hosted
wrapper inside the payload.

## Managed Verify

Besides LLVM's own verifier, MVM now has a separate managed verifier layer.

Today it rejects a small set of IR constructs that would make future precise GC
hard to reason about:

- missing debug compile units
- defined functions without debug subprogram metadata
- `ptrtoint`
- `inttoptr`
- `addrspacecast`
- inline assembly
- module-level inline assembly
- raw libc allocators such as `malloc`, `calloc`, `realloc`, and `free`

This is intentionally narrow for now. The point is to establish a dedicated
policy stage that can grow stricter when GC metadata and object rules land.

## Managed Memory ABI

Managed-mode memory is now expected to go through MVM-owned symbols:

- `__mvm_malloc()`
- `__mvm_array_malloc(element_count)`
- `__mvm_array_length(ptr)`

`lona-ir` now emits explicit `!lona.alloc.type` metadata on managed
allocations. MVM treats that metadata as mandatory, rewrites allocation calls
to internal typed helpers, and derives object size, alignment, and pointer-slot
layout from the resolved LLVM type instead of trusting source-level size or
alignment arguments.

At the source-library level, MVM also treats these as different pointer
surfaces: single-object allocation stays `T*`, while array allocation is
modeled as `T[*]`.

Managed code already knows the element type statically, so runtime allocation
APIs no longer accept user-supplied size, element-size, or alignment values
that could conflict with `alloc type` metadata.

What is implemented now:

- runtime allocation ABI
- injected static array bounds checks
- injected dynamic array bounds checks for GEP-based accesses rooted in
  `__mvm_array_malloc`
- root-based managed-state propagation and IR metadata annotation
- verifier enforcement against raw libc allocators

What is not implemented yet:

- full managed lowering around source-level dynamic array/library surface
- broader pointer provenance tracking for more complex derived managed-array
  pointer shapes
- frontend lowering of managed references into LLVM GC-managed pointer address
  spaces, so current safepoints do not yet carry precise live-root relocation
  metadata for ordinary `ptr` values

## Fault Handling Boundary

The long-term direction is to support recoverable managed traps for explicitly
modeled cases such as null traps or guard-page overflow.

That does not imply a guarantee that arbitrary in-process native memory
corruption can be recovered safely. For that stronger guarantee, process
isolation is still the correct boundary.

## Build And Run

```bash
make all
../lona/build/lona-ir --emit linked-bc --verify-ir -g \
    examples/hello.lo build/examples/hello.bc
build/mvm build/examples/hello.bc
```

Run the bundled smoke coverage:

```bash
make test
```

For managed payloads, `mvm` accepts `-O1`, `-O2`, and `-O3`. `-O0` is rejected.

The smoke samples now use `lona` source directly:

- [hello.lo](examples/hello.lo)
- [static_array_ok.lo](examples/static_array_ok.lo)
- [static_array_oob.lo](examples/static_array_oob.lo)
- [runtime_array_api.lo](examples/runtime_array_api.lo)
- [runtime_array_oob.lo](examples/runtime_array_oob.lo)
- [runtime_argv.lo](examples/runtime_argv.lo)
- [managed_state.lo](examples/managed_state.lo)
- [runtime_raw_malloc.lo](examples/runtime_raw_malloc.lo)

The remaining raw LLVM IR fixture is:

- [invalid_ptr_cast.ll](examples/invalid_ptr_cast.ll)

Reason:

- current stable `lona` rejects integer-to-pointer casts before LLVM IR emission,
  so that specific negative case still cannot be authored as valid `.lo` source
