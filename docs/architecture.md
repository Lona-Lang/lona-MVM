# MVM Architecture Draft

## 1. Scope

This document describes the first implementation shape for `lona-mvm`.

It does not define final GC, reflection, or metadata semantics. Those features
depend on a stable managed runtime ABI first.

## 2. Core Model

The upstream `lona` architecture already separates:

- shared frontend
- HIR
- `native lowering`
- `managed lowering`

This repository should continue that split instead of trying to execute current
native-oriented output inside another process.

The intended pipeline is:

```text
.lo source
  -> lona frontend
  -> HIR
  -> managed lowering
  -> managed payload
  -> mvm loader
  -> JIT
  -> runtime services
```

## 3. Important Distinctions

### 3.1 Target mode vs delivery mode

There should be only one managed target mode.

Inside that target, the runtime supports two delivery modes:

- external payload execution: `mvm run <payload>`
- self-bundled executable: launcher plus embedded payload

These are distribution choices, not new compiler targets.

### 3.2 Canonical payload vs debug input

Proposal:

- canonical runtime payload: LLVM bitcode bundle
- optional debug input: textual LLVM IR

Bitcode should be the source of truth for runtime packaging. Text IR may be
accepted by debug tools, but the main runtime path should not depend on it.

## 4. Managed Payload Proposal

The MVM needs a stable package format between `managed lowering` and runtime
loading.

Minimum payload fields:

- format version
- runtime ABI version
- target triple
- optimization level
- entry symbol
- module list
- per-module bitcode bytes

Optional fields for later milestones:

- source map or debug metadata index
- reflection metadata
- GC metadata
- stack map data
- embedded resources

At a high level, the payload should look like:

```text
header
manifest
module bitcode section(s)
optional metadata section(s)
```

## 5. Two Startup Profiles

### 5.1 `run`

This is the first path to implement.

Responsibilities:

- read payload from disk
- validate format and runtime ABI version
- materialize LLVM modules
- link or import them into the JIT
- resolve runtime intrinsics
- invoke the managed entry point

This path is the easiest way to validate the runtime contract.

### 5.2 `bundle`

This path should not introduce a second execution engine.

Instead:

- keep the same runtime core
- wrap it in a launcher executable
- embed the payload into a known section or append it with a footer marker
- on startup, detect the embedded payload and hand it to the same loader used by
  `run`

That keeps packaging logic separate from JIT logic.

## 6. Runtime Subsystems

The first implementation should be split into these components.

### 6.1 Package loader

Responsibilities:

- read payload header and manifest
- validate version compatibility
- expose module bytes and metadata to the rest of the runtime

### 6.2 JIT engine

Responsibilities:

- own LLVM context objects needed for loading
- ingest module bitcode
- perform final link/JIT materialization
- expose a typed entry invocation path

The exact LLVM API can still change, but the runtime should hide it behind a
small local interface.

### 6.3 Runtime ABI layer

Responsibilities:

- define symbols that managed-lowered code may call
- isolate runtime services from raw LLVM/JIT details
- become the future home for allocation, metadata lookup, and diagnostics

This boundary is more important than any specific GC choice in the first stage.

#### Managed memory ABI v0

The first concrete runtime ABI that this repository now needs is managed memory.

For managed mode, the runtime should reserve allocation to MVM-owned entry
points instead of letting user code call arbitrary libc allocators directly.

Recommended v0 surface:

- `__mvm_malloc(size, alignment)`
- `__mvm_array_malloc(element_size, element_count, alignment)`
- `__mvm_free(ptr)`
- `__mvm_array_free(ptr)`
- `__mvm_array_length(ptr)`

Design intent:

- single-object allocation and array allocation are distinct ABI paths
- source-level bindings should reflect that split as `T*` vs `T[*]`
- dynamic arrays carry hidden metadata owned by MVM
- managed code receives the payload pointer, not the header pointer
- runtime metadata layout must not depend on a particular system malloc
  implementation

For dynamic arrays, `__mvm_array_malloc` should allocate:

```text
[hidden mvm header][aligned array payload]
```

The hidden header is the source of truth for:

- element count
- allocation kind

That metadata exists so IR injection can do dynamic array bounds checks without
relying on allocator-specific layout.

#### Static vs dynamic bounds checks

The intended lowering split is:

- static arrays
  - inject an IR compare against the compile-time constant bound
- dynamic arrays
  - inject checks against MVM-owned runtime metadata
  - by reading length metadata from `__mvm_array_length`

This repository now implements:

- static array bounds-check injection for aggregate GEPs
- dynamic array bounds-check injection for GEPs whose base pointer can be proven
  to come from `__mvm_array_malloc`

The current implementation is intentionally basic. More complex pointer
provenance and source-level managed lowering can be tightened later.

### 6.4 Embedding/bootstrap layer

Responsibilities:

- command-line `run` mode
- bundled executable startup mode
- common entry argument handling

## 7. Runtime Thread Model

The MVM should not assume that one thread does everything forever.

Even before GC lands, it is useful to separate:

- mutator thread
  - executes JITed managed code
- VM/runtime thread
  - owns global runtime coordination
  - maintains code maps and metadata indexes
  - becomes the future coordinator for safepoints and GC

For the current small prototype, this can still be implemented incrementally.
The important point is to reserve the architecture now.

### 7.1 Recommended early topology

The recommended near-term layout is:

```text
main thread
  -> startup / CLI / shutdown

VM/runtime thread
  -> global runtime state
  -> code registration
  -> source mapping tables
  -> future safepoint coordinator

mutator-0
  -> execute current JIT program
```

Later expansions can add:

- GC worker threads
- background JIT or optimization threads
- profiling or watchdog threads

### 7.2 Why the VM thread is still useful before GC

Even without collection yet, a VM/runtime thread is a good home for:

- code address range registration
- `PC -> function -> source` mapping
- trap-site registry
- runtime state transitions
- future stop-the-world requests

This keeps fault-handling and metadata logic out of the mutator fast path.

The current prototype now follows this topology incrementally:

- main thread performs CLI, bitcode loading, verification, and JIT setup
- mutator thread performs the managed entry invocation
- VM/runtime thread stays resident so future safepoint and GC coordination has
  a fixed home

The first GC-facing compiler step should be modeled as "GC-ready IR", not as a
full collector implementation. In practice that means:

- mark managed functions with an LLVM GC strategy such as `statepoint-example`
- provide `gc.safepoint_poll`
- run late safepoint/statepoint rewriting passes
- postpone precise root relocation until frontend lowering can distinguish
  managed references from ordinary raw pointers

Between the current raw `ptr` world and that future relocation-aware lowering,
the runtime can still do a useful intermediate step: root-based managed-state
analysis. In this model:

- `__mvm_malloc` seeds managed-object provenance
- `__mvm_array_malloc` seeds managed-array provenance
- propagation runs over SSA, slot loads/stores, `phi`, `select`, `gep`, direct
  call arguments, and direct call returns
- the result is written back as IR metadata so later passes can inspect the
  inferred managed state without requiring frontend-emitted managed pointer
  types yet

## 8. Fault Handling Model

The MVM should distinguish between:

- recoverable managed traps
- fatal native corruption

Those are not the same problem.

### 8.1 Recoverable managed traps

A recoverable trap is a hardware or runtime fault that the VM has already
modeled ahead of time.

Typical examples:

- null dereference checks implemented as implicit traps
- guard-page based stack overflow
- explicit runtime trap points inserted by managed lowering

For these cases, the intended flow is:

1. install platform fault handlers such as `sigaction` on Unix
2. capture fault address and instruction pointer from OS context
3. check whether the faulting PC belongs to a registered recoverable trap site
4. if yes, redirect execution to a runtime stub
5. let runtime code turn that into a managed exception or runtime error

The VM/runtime thread should own the metadata needed by step 3, but the actual
context rewrite still happens on the faulting thread.

### 8.2 What this model does not guarantee

This model does not mean that arbitrary invalid memory accesses are safely
recoverable.

In particular:

- writes into the wrong but still mapped address may not trap at all
- corrupted in-process runtime state may already be unrecoverable
- faults outside registered trap semantics should be treated as fatal

Therefore the VM/runtime thread is a coordination mechanism, not a substitute
for process isolation.

### 8.3 Architectural implication for managed code

If the MVM wants low-overhead recoverable faults later, managed verification and
managed lowering must keep the execution model constrained.

That means continuing to avoid or tightly control:

- arbitrary raw pointer arithmetic
- integer/pointer round trips
- inline assembly
- opaque native memory manipulation in managed code
- direct use of raw libc allocation APIs in managed code

The current verifier direction in this repository is consistent with that goal.

## 9. Integration Points With `lona`

Based on upstream docs, the compiler side still needs explicit managed support.

That work should eventually include at least:

- `TargetMode` in session and module data
- `ModuleInterface.targetMode`
- `ModuleArtifact.targetMode`
- import-time mode validation
- capability checks during resolve/typecheck
- `managed lowering` as a separate backend path

This repository can start before all of that lands upstream, but its runtime ABI
should be designed to plug into those compiler contracts later.

## 10. Recommended First Milestones

### M0: Runtime shell

Goal:

- create a minimal `mvm run <bitcode-or-payload>` executable
- load one module
- find and invoke a known entry symbol

Out of scope:

- GC
- reflection
- self-bundling

### M1: Managed payload format

Goal:

- replace ad hoc raw input loading with a versioned payload container
- support multiple modules and a manifest

### M2: Bundled executable

Goal:

- produce a single executable with embedded payload
- reuse the exact same loader and JIT path as `run`

### M3: Runtime ABI v0

Goal:

- define runtime service symbols for allocation and diagnostics
- make managed lowering target those symbols explicitly

### M4: GC-ready execution points

Goal:

- introduce safepoint strategy
- define where stack maps or equivalent metadata will come from
- establish supervisor-owned trap metadata tables
- define which traps are recoverable versus fatal

GC should start only after M3 is stable.

### M5: Recoverable trap path

Goal:

- install OS fault handlers
- map faulting PCs back to JIT code metadata
- recover only from explicitly modeled trap sites

Non-goal:

- promising recovery from arbitrary native memory corruption

## 11. Practical Recommendation

If the first objective is to make progress fast, do not start with GC.

Start with this narrower path:

1. accept bitcode-based managed payloads
2. JIT one entry function reliably
3. make `run` mode stable
4. introduce supervisor-owned code/source mapping
5. add bundled executable mode
6. only then add managed runtime services
7. after that, add recoverable trap support for explicit managed cases

That order keeps the project moving while avoiding early commitment to the wrong
object model.
