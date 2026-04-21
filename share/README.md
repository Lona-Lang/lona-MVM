# Share

This directory stores source-level shared modules for `lona-mvm`.

Current contents:

- [mvm_memory.lo](mvm_memory.lo): repo-local draft of the MVM managed memory
  library, authored in `lona`

Notes:

- this is a repository-local source library draft
- it describes the intended managed allocation surface used by MVM
- current bundled `lona` references do not define managed-mode FFI as a stable
  public language contract yet
- current bundled references also do not document a stable `alignof`-style
  surface, so alignment stays explicit in this draft API
