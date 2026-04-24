# Share

This directory stores source-level shared modules for `lona-mvm`.

Current contents:

- [managed/mem.lo](managed/mem.lo): managed memory surface for `mvm`
- [native/mem.lo](native/mem.lo): native memory surface with the same
  user-facing API
- [mvm_memory.lo](mvm_memory.lo): older managed-only draft module kept for
  reference during the transition

The intended workflow is:

- user code imports `mem`
- managed builds add `-I share/managed`
- native builds add `-I share/native`

That keeps algorithm/data-structure code unchanged while switching the backing
allocation mode only through the build command.

Notes:

- this is a repository-local source library draft
- the shared surface is now `newObject[T]`, `newArray[T]`,
  `arrayLength[T]`, and `freeObject[T]`
- managed mode lowers those APIs to `__mvm_malloc`, `__mvm_array_malloc`, and
  `__mvm_array_length`; `freeObject[T]` is intentionally a no-op there
- native mode keeps the same source API and hides raw `malloc`
- object allocation no longer accepts explicit size/alignment arguments; MVM
  derives them from compiler-emitted alloc-type metadata
- array allocation only accepts element count; element size and alignment come
  from the same alloc-type metadata
- current bundled `lona` references do not define managed-mode FFI as a stable
  public language contract yet
