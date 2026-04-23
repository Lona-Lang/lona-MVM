# Share

This directory stores source-level shared modules for `lona-mvm`.

Current contents:

- [mvm_memory.lo](mvm_memory.lo): repo-local draft of the MVM managed memory
  library, authored in `lona`

Notes:

- this is a repository-local source library draft
- it describes the intended managed allocation surface used by MVM
- it now distinguishes single-object memory pointers from array pointers:
  `__mvm_malloc` returns `T*`, while `__mvm_array_malloc` returns `T[*]`
- object allocation no longer accepts explicit size/alignment arguments; MVM
  derives them from compiler-emitted alloc-type metadata
- array allocation only accepts element count; element size and alignment come
  from the same alloc-type metadata
- current bundled `lona` references do not define managed-mode FFI as a stable
  public language contract yet
