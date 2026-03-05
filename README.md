# allocazam

`allocazam` is a C++23 custom allocator project focused on predictable pool behavior and low-overhead allocation paths for allocator-aware containers.

This repository is under active development (read: "I'm tinkering"); internals and APIs may continue to change (read: "break").

## Architecture Overview

The implementation is split into three core layers:

- `lib/allocazam.hpp`: primary allocator template `allocazam<T, Mode>`
- `lib/types.hpp`: low-level storage primitives (`node_t`, `chunk_t`) and alignment utilities
- `lib/runner.hpp`: contiguous run allocator used for multi-element allocation paths

## Memory Modes

`memory_mode` currently has three modes:

- `fixed`: single heap-backed chunk, no growth after construction
- `dynamic`: heap-backed chunks with growth when capacity is exhausted
- `noheap`: caller-provided backing span, no allocator-owned heap allocation in pool paths

Mode selection is a template argument, and mode-specific behavior is constrained at compile time with concepts and `requires`.

## Pool Layer (`allocazam`)

At the pool level, allocation of individual objects is node-based:

- free nodes are tracked with an intrusive free list
- object storage is reused in-place
- construction/destruction are separated from raw slot acquisition/release
- growth is mode-dependent (`dynamic` can add chunks, fixed-like modes cannot)

The allocator also exposes allocator-traits compatibility hooks (`rebind`, equality operators, `allocate_at_least` support path) to integrate with standard containers.

## Node and Chunk Layer (`types.hpp`)

- `node_t<T>` defines raw storage sized/aligned for either `T` or free-list linkage metadata
- `chunk_t<T, owns_memory>` represents contiguous node regions
- ownership is encoded at compile time (`owns_memory`), so external buffers and owned buffers share one structural model

This keeps steady-state node operations simple while preserving mode-specific ownership semantics.

## Runner Allocation Layer (`runner.hpp`)

For larger contiguous requests (`n > 1` style paths), run allocation is handled by `allocazam::run_allocator`:

- run headers encode size and coalescing flags
- free runs are bucketed (linear lower bins + logarithmic upper bins)
- non-empty bins are tracked with bitmasks for fast candidate lookup
- splitting/coalescing maintains reuse and limits external fragmentation

This layer is designed for contiguous region management, complementing the single-node free-list path in `allocazam`.

## Design Intent

The current design targets:

- explicit behavior by mode
- predictable allocation failure semantics in bounded modes
- low metadata overhead in hot paths
- composable internals that can evolve independently as performance work continues
