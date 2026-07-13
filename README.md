# allocazam

[![CI](https://github.com/dr7ana/allocazam/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/dr7ana/allocazam/actions/workflows/ci.yml)

`allocazam` is a C++23 allocator library for bounded, low-latency storage. Its standard-allocator interface separates two compile-time decisions:

- `memory_mode` selects where backing memory comes from and whether it can grow.
- `allocation_model` selects how that backing is divided among live allocations.

The repository is under active development; internals and APIs may change freely.

## Configuration

Memory modes:

- `memory_mode::fixed`: Allocazam owns eagerly acquired, non-growing backing.
- `memory_mode::dynamic`: Allocazam owns backing and may add chunks.
- `memory_mode::noheap`: the caller supplies backing and Allocazam never owns or releases it.

Allocation models:

- `allocation_model::suballocated`: the existing node-pool and runner design supports multiple simultaneous allocations.
- `allocation_model::exclusive`: one contiguous region supports exactly one live nonzero allocation at a time and is reusable after release.

Supported combinations:

| Memory mode | `suballocated` | `exclusive` |
| --- | --- | --- |
| `fixed` | yes | yes |
| `dynamic` | yes | no |
| `noheap` | yes | yes |

The standard state and allocator template order is:

```cpp
allocazam_std_state<T, Memory, Allocation, Huge>
allocazam_std_allocator<T, Memory, Allocation, Huge>
```

`Allocation` defaults to `suballocated` and `Huge` defaults to `disabled`. `noheap` supports only `huge_pages::disabled`; page provenance for caller memory is deliberately outside Allocazam policy.

## Exclusive allocation

Exclusive allocation is intended for a vector-like buffer, bounded ring, frame table, scratch region, or another object that needs one stable contiguous allocation. It has constant-time claim/release bookkeeping, stores no metadata in payload bytes, and contains no pool, runner, bin, scan, TLS state, registry, owner routing, lock, or atomic.

The state owns or borrows the byte region. The allocator is a non-owning, one-pointer handle bound explicitly to that state:

```cpp
using state_t = allocazam::allocazam_std_state<
        record,
        allocazam::memory_mode::fixed,
        allocazam::allocation_model::exclusive>;

using allocator_t = allocazam::allocazam_std_allocator<
        record,
        allocazam::memory_mode::fixed,
        allocazam::allocation_model::exclusive>;

state_t state{1024}; // element capacity
allocator_t allocator{state};
```

Exclusive allocators have no default constructor. States are non-copyable and non-movable so allocator resource pointers remain stable. Allocator handles themselves are copyable and movable; copying or moving a handle preserves resource identity and does not clone or own the state.

### Standard containers: reserve first

A standard container normally performs no allocation in its constructor. Its first growth may request only one element, which would claim the exclusive region for that exact allocation and prevent replacement growth.

Portable vector usage reserves the complete intended capacity before inserting anything:

```cpp
using vector_t = std::vector<record, allocator_t>;

vector_t records{allocator};
records.reserve(state.capacity());

for (size_t i = 0; i < state.capacity(); ++i) {
    records.emplace_back(/* ... */);
}
```

Do not depend on a standard library calling `allocate_at_least()`. The explicit full-capacity `reserve()` is the contract.

Containers that require simultaneous allocations, including node containers and ordinary unordered containers, are not compatible with one exclusive resource. Use `suballocated` for them. A vector can be moved while its state remains alive. Ordinary nonempty copy construction selects the same occupied resource and therefore fails; allocator-extended copy construction into a distinct state is the supported copy path. Exception guarantees for a failed container replacement are determined by that standard-library implementation.

### Direct bounded storage

A ring or other direct consumer can claim its whole region without container growth behavior:

```cpp
record* storage = allocator.allocate(state.capacity());
// Construct, use, and destroy live records as needed.
allocator.deallocate(storage, state.capacity());
```

Only one nonzero claim may be live. A second claim throws `std::bad_alloc`. Once the first claim is released, the same backing is immediately reusable. `allocate_at_least(n)` reports the full representable capacity and accepts any standard-permitted deallocation count from `n` through the returned count.

Exclusive state and backing access is externally serialized. A claim may be handed between threads through normal synchronization, but concurrent operations on one state are not supported.

### Noheap exclusive

Noheap state aligns forward inside the supplied span, truncates the tail to complete elements, and acquires no memory:

```cpp
alignas(std::max_align_t) std::array<std::byte, 64 * sizeof(record)> backing{};

using state_t = allocazam::allocazam_std_state<
        record,
        allocazam::memory_mode::noheap,
        allocazam::allocation_model::exclusive>;
using allocator_t = allocazam::allocazam_std_allocator<
        record,
        allocazam::memory_mode::noheap,
        allocazam::allocation_model::exclusive>;

state_t state{std::span<std::byte>{backing}};
allocator_t allocator{state};

std::vector<record, allocator_t> records{allocator};
records.reserve(state.capacity());
```

The caller-provided backing must outlive the state, every bound allocator, and every live allocation. Allocazam neither frees nor initializes the span. Passing a subspan is the way to impose a smaller capacity.

This mode provides groundwork for embedded deployments where storage may come from static arrays, linker regions, shared memory, device-specific RAM, or another externally managed arena. Allocazam does not infer that memory's provenance or add exception-free platform policy.

## Suballocated allocation

`suballocated` supports general standard-container allocation patterns:

- single-object allocations use an intrusive node pool;
- multi-element contiguous allocations use the runner;
- fixed mode fails when its pool/runner backing is exhausted;
- dynamic mode can grow;
- noheap mode divides caller backing between a node pool and runner;
- default heap-backed allocators use per-thread states and owner-routed cross-thread release;
- explicit states retain confined, caller-managed semantics.

The raw pool API remains independent of `allocation_model`:

```cpp
allocazam::allocazam<record, allocazam::memory_mode::fixed> pool{4096};
```

## Huge pages

`huge_pages::enabled` means explicit Linux 2 MiB hugetlb mappings using `MAP_HUGETLB | MAP_HUGE_2MB`. There is no ordinary-page fallback.

For `suballocated`, huge pages back allocator-owned runner chunks; the node pool remains ordinary-page-backed. For `fixed + exclusive`, huge pages back the single owned region. Logical capacity remains the requested usable byte count, while `mapping_bytes()` reports the actual smallest containing 2 MiB multiple.

```cpp
using state_t = allocazam::allocazam_std_state<
        char,
        allocazam::memory_mode::fixed,
        allocazam::allocation_model::exclusive,
        allocazam::huge_pages::enabled>;

using allocator_t = allocazam::allocazam_std_allocator<
        char,
        allocazam::memory_mode::fixed,
        allocazam::allocation_model::exclusive,
        allocazam::huge_pages::enabled>;

state_t state{2u << 20};
allocator_t allocator{state};
```

An exact 2 MiB exclusive request maps exactly 2 MiB. A request one byte larger maps 4 MiB but exposes only the requested logical capacity. Mapping or alignment failure throws `std::bad_alloc`.

## Implementation layers

- `lib/allocazam.hpp`: raw pool plus standard state/allocator specializations.
- `lib/exclusive_resource.hpp`: non-owning exclusive claim ledger.
- `lib/owned_memory.hpp`: shared ordinary/hugetlb backing acquisition.
- `lib/runner.hpp`: size-segregated contiguous run allocator for `suballocated`.
- `lib/types.hpp`: storage primitives, modes, policies, and checked arithmetic.

The runner keeps boundary metadata, linear/logarithmic bins, bitmask lookup, splitting, coalescing, expansion, and owner routing. Exclusive allocation intentionally bypasses that machinery when a consumer needs only one stable region.
