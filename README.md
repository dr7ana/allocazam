# allocazam

[![CI](https://github.com/dr7ana/allocazam/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/dr7ana/allocazam/actions/workflows/ci.yml)

`allocazam` is a C++23 hybrid freelist allocator with size-segregated bins, optimized for predictable, deterministic low-latency allocation.

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

## Huge Pages

Contiguous run allocation can also be configured at compile time with `huge_pages`:

- `huge_pages::disabled`: default behavior
- `huge_pages::enabled`: explicit Linux hugetlb mappings for allocator-owned runner chunks

Current scope:

- `huge_pages` is a template parameter on `allocazam::runner::allocator`, `allocazam_std_state`, and `allocazam_std_allocator`
- `enabled` currently means explicit `2 MiB` huge pages via `MAP_HUGETLB | MAP_HUGE_2MB`
- there is no fallback path; if hugetlb is enabled and the system is not configured for it, allocation fails
- this only affects runner-owned contiguous allocations; the single-object node pool path remains normal-page-backed
- caller-provided external backing spans are unchanged

For std-allocator integration, hugetlb-enabled allocators must be constructed from explicit state:

```cpp
using state_t = allocazam::allocazam_std_state<
        int,
        allocazam::memory_mode::dynamic,
        allocazam::huge_pages::enabled>;

using alloc_t = allocazam::allocazam_std_allocator<
        int,
        allocazam::memory_mode::dynamic,
        allocazam::huge_pages::enabled>;

state_t state{4096, 2u << 20};
alloc_t alloc{state};
```

The default-constructed std allocator remains available only for `huge_pages::disabled`.

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

For larger contiguous requests (`n > 1` style paths), run allocation is handled by `allocazam::runner::allocator`:

- run headers encode size and coalescing flags
- free runs are bucketed (linear lower bins + logarithmic upper bins)
- non-empty bins are tracked with bitmasks for fast candidate lookup
- splitting/coalescing maintains reuse and limits external fragmentation
- allocator-owned runner chunks can optionally be backed by explicit `2 MiB` hugetlb mappings

This layer is designed for contiguous region management, complementing the single-node free-list path in `allocazam`.

## Design Intent

The current design targets:

- explicit behavior by mode
- predictable allocation failure semantics in bounded modes
- low metadata overhead in hot paths
- composable internals that can evolve independently as performance work continues
