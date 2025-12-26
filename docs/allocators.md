# Allocators: `linear_allocator` Arena Allocation Inside a Segment

This library provides `linear_allocator<Tag, OffsetT>`, a linear arena allocator intended for allocating storage from a contiguous byte range that is itself part of a position-independent segment. The allocator is monotonic. Successful allocations advance a cursor forward and never move it backward. There is no per-allocation free. Reclamation is an explicit arena-wide operation performed by `reset()`.

The allocator is designed to support two simultaneous needs that are common in shared-memory and memory-mapped-file designs: first, fast concurrent reservation of disjoint storage ranges without external locks; second, the ability to return references that can be stored inside the segment in position-independent form, using segment-relative `offset_ptr` handles.

## The Arena Model

The library provides a thread-safe linear (arena) allocator. Allocations are fast atomic pointer bumps. Deallocation is a no-op. The entire arena is cleared at once via `reset()`. This model is designed for high-throughput, frame-based, or batch-processing IPC workloads where object lifetime is tied to the arena, not individual items.

The arena model is a lifetime policy, not a micro-optimization. When you allocate from a linear arena, you are making a statement that individual object lifetimes are not tracked by the allocator and that memory pressure is managed at a coarser granularity. If you allocate an object and later stop using it, those bytes remain consumed until the next `reset()`. If an STL container grows and discards prior buffers as part of reallocation, those discarded buffers remain consumed until the next `reset()`. There is no mechanism in this allocator to return those bytes to the available pool.

A correct integration therefore requires an explicit definition of the arena lifetime boundary. Typical boundaries are a “frame” in a real-time system, a “batch” in a pipeline, a “message epoch” in an IPC protocol, or a “transaction attempt” in a producer that can restart work. The allocator does not infer these boundaries. The boundary is where you are allowed to call `reset()`, and the boundary must be chosen so that no live references to arena-allocated objects remain reachable after reset.

This allocator intentionally does not attempt to provide “mostly linear but sometimes free” behavior. That hybrid design is where shared-memory allocators accumulate complexity and hidden invariants. Here, allocation is monotonic; reclamation is whole-arena; anything else is the responsibility of a higher-level design.

## API Reference

`linear_allocator<Tag, OffsetT>` is parameterized by a `Tag` and an `OffsetT`. `Tag` binds the allocator’s handle types to the segment anchoring system. `OffsetT` is the integer representation used in those handles. If you persist the segment bytes or share them across builds, both `Tag` choice and `OffsetT` choice must be treated as part of the segment schema because they determine how references are interpreted.

The allocator provides raw-pointer allocation for immediate in-process use and handle-returning allocation for storing references inside the segment. A raw pointer is process-local and is not a valid persistent representation. A handle is a segment-relative `offset_ptr` and is intended to be stored in shared bytes.

### Types

The allocator defines `handle<T>` as `shm::segment_offset_ptr<T, Tag, OffsetT>`. A `handle<T>` is a segment-relative position-independent reference. Its decoding base is `segment_base<Tag>`.

The allocator defines `void_handle` as `handle<void>`.

The allocator defines `stl_allocator<T>`, a standard-library allocator adapter that forwards allocations to the arena and treats deallocation as a no-op.

### Construction and segment base binding

`linear_allocator(void* start, std::size_t size) noexcept` constructs an arena that allocates from `[start, start + size)`. It also establishes the segment base for `Tag` by calling `shm::segment_base<Tag>::set(start)` in the current process. This construction form is appropriate when the arena begins at the segment base and you want handles to be interpreted relative to that same base.

`linear_allocator(void* segment_base, void* arena_start, std::size_t arena_size) noexcept` constructs an arena that allocates from `[arena_start, arena_start + arena_size)` but interprets handles relative to `segment_base`. It establishes `shm::segment_base<Tag>::set(segment_base)` in the current process. This construction form is appropriate when the segment has a header or other fixed subregions and the arena begins after those regions, but you still want all handles to share a single coordinate system based on the mapped segment base.

The segment base is process-local state. It is not stored in the shared bytes. Any process that decodes `handle<T>` values must establish the same segment base for the same `Tag` before decoding. If the base is not established, decoding is program-logic error; the implementation may assert in debug builds and may decode garbage in release builds.

If a program maps multiple independent segments simultaneously, it must use distinct `Tag` types per independent segment base. Reusing the same `Tag` for multiple active segment bases makes handle decoding ambiguous by construction.

### `void* alloc(std::size_t n, std::size_t alignment = alignof(std::max_align_t)) noexcept`

`alloc` reserves `n` bytes from the arena and returns a pointer aligned to `alignment`. On failure it returns `nullptr` and consumes nothing.

`n == 0` returns `nullptr`.

`alignment == 0` is treated as `1`.

The allocator accepts both power-of-two and non-power-of-two alignments. Power-of-two alignment is implemented by rounding up using a mask. Non-power-of-two alignment is implemented by modular arithmetic.

The allocator is monotonic. Any padding inserted to satisfy alignment permanently consumes arena space until reset.

The returned pointer is meaningful only in the current process mapping. It is a virtual address. It must not be written into the shared segment as an embedded pointer. If a reference must be stored in the segment, convert it to a `handle<T>` (or return a handle directly using `alloc_handle` or `make_handle`).

### `void_handle alloc_handle(std::size_t n, std::size_t alignment = alignof(std::max_align_t)) noexcept`

`alloc_handle` is the handle-returning analog of `alloc`. It reserves `n` bytes and returns a `handle<void>` that refers to the allocated region in segment-relative form.

On failure it returns a null handle, meaning a handle whose stored representation is the null encoding and whose `get()` decodes to `nullptr` after the base is established.

A `handle<void>` returned by `alloc_handle` is a position-independent representation suitable for embedding into shared structures. The underlying bytes are still untyped storage; object lifetime rules remain the responsibility of the caller.

### `template <class T> T* allocate(std::size_t count = 1) noexcept`

`allocate<T>(count)` reserves storage for `count` objects of type `T` and returns a `T*` aligned to `alignof(T)`. It does not construct objects.

`count == 0` returns `nullptr`.

If `count * sizeof(T)` overflows `std::size_t`, allocation fails and returns `nullptr`.

If the arena cannot satisfy the request, allocation fails and returns `nullptr`.

This function is storage-only. If `T` is not an implicit-lifetime type under your project’s rules, you must begin lifetime explicitly via placement new before accessing the returned memory as a `T`.

### `template <class T> handle<T> allocate_handle(std::size_t count = 1) noexcept`

`allocate_handle<T>(count)` allocates storage for `count` objects of type `T` and returns a segment-relative handle to the first element.

This is the handle-returning analog of `allocate<T>`. It is appropriate when the allocation must be referenced from inside shared bytes.

It does not construct objects. It returns storage in typed form for convenience, but object lifetime rules remain the responsibility of the caller.

### `template <class T, class... Args> handle<T> make_handle(Args&&... args)`

`make_handle<T>(args...)` allocates storage for one `T`, constructs `T` in place using placement new, and returns a segment-relative handle to the constructed object.

If allocation fails, it returns a null handle.

If `T` construction throws, the exception propagates and the arena space remains consumed. The allocator does not roll back the cursor on construction failure. This is not an accident. Rollback is not generally correct under concurrent allocation and is inconsistent with the arena model. If throwing constructors are unacceptable, do not construct such types in the arena.

Objects constructed via `make_handle` are not registered for destruction. `reset()` does not call destructors. If `T` has a non-trivial destructor whose effects are required for correctness, you must not allocate such `T` in this arena unless you have an explicit higher-level destruction pass that runs before reset. Under the position-independent segment model, the normal posture is that resident objects do not own external resources and have trivial or semantically irrelevant destructors.

### `void reset() noexcept`

`reset()` sets the allocator cursor back to zero. The arena becomes empty and subsequent allocations reuse the same bytes from the beginning.

`reset()` does not zero memory. It only rewinds the cursor. Any previously returned raw pointers and any previously returned handles become stale in the sense that their storage may be overwritten by later allocations. The allocator does not and cannot invalidate those references in-band. Correctness depends on calling `reset()` only at a boundary where no references into the arena remain reachable.

`reset()` does not call destructors for objects constructed in the arena.

### `void secure_reset() noexcept`

`secure_reset()` is a convenience operation that clears the used prefix of the arena by writing zero bytes over it and then rewinds the cursor to zero.

This function exists to reduce data remanence between arena epochs. It is not a durability mechanism, not a cryptographic erasure guarantee, and not a crash-consistency protocol. It is a byte wipe.

`secure_reset()` shares the same safety constraints as `reset()`. It must not run while other threads or processes are accessing arena-allocated objects, because it overwrites the storage.

### `std::size_t used() const noexcept` and `std::size_t capacity() const noexcept`

`used()` returns the current cursor value in bytes. This is the number of bytes reserved since the last reset, including alignment padding. It is not “live object bytes,” and it is not reduced by container deallocation.

`capacity()` returns the fixed arena size in bytes.

### `bool owns(const void* p) const noexcept`

`owns(p)` returns whether `p` lies within the arena address range in the current process mapping. It is useful for debug assertions and internal sanity checks. It is not a security boundary and it does not imply that `p` points to a live object.

## Thread Safety Guarantee

All allocation functions are lock-free and thread-safe. Multiple threads or processes can allocate from the same `linear_allocator` instance without external locking. The `reset()` function is not thread-safe and must be called only when no other threads are accessing the allocator.

This guarantee is specifically about allocation as reservation of disjoint storage ranges. The allocator implements allocation by performing a CAS loop on an atomic cursor. Each successful allocation commits a unique cursor advancement and therefore reserves a unique non-overlapping byte range. If contention exists, losing threads retry with an updated cursor value.

The guarantee does not extend to the contents you place into allocated storage. Allocation reserves bytes. It does not publish initialized objects. If you allocate and then write an object that another thread or process will read, you must provide a publication protocol. A typical protocol is to fully initialize the object, then store the handle to it with release semantics, and require readers to load that handle with acquire semantics before dereferencing. The allocator does not insert fences around your object initialization, and it does not attempt to make partially initialized objects unobservable.

The guarantee “across processes” has a mechanically necessary precondition: the allocator control state must be shared. The allocator’s cursor is the single authority that prevents overlapping allocations. If each process constructs its own allocator object in private memory, each process has its own cursor, and those cursors will advance independently, producing overlapping allocations into the same shared arena bytes. Cross-process allocation therefore requires that the cursor state be located in shared memory and mapped consistently by all participating processes, or that a single process act as the allocator authority. If you want true multi-process concurrent allocation into the same arena, place the allocator instance itself in the shared segment so that its atomic cursor is a shared atomic object and all processes operate on the same cursor.

`reset()` is excluded from the thread-safety guarantee because it rewinds the cursor and enables reuse of already-allocated bytes. If any thread or process can still access those bytes, reset creates use-after-recycle behavior at the arena level. If any allocation can race with reset, reset can cause overlapping allocations. Correct usage requires that reset occur only under a quiescent condition that you enforce, typically by a global epoch barrier, a control-plane lock, or a single-writer policy.

## STL Integration (Usage Example)

This section demonstrates how to use the allocator through `stl_allocator<T>`, which satisfies the standard allocator interface and forwards allocations to the arena. The mechanically relevant behavior is that `deallocate` is a no-op. Standard containers will function as containers, but their memory-return behavior is intentionally disabled. Memory consumption is monotonic until reset.

```cpp
#include "shmTypes.hpp"
#include <vector>
#include <string>
#include <iostream>

// Use a tag for the segment
struct MyTag {};

int main() {
    // 1. Create a memory region for the allocator and its data
    alignas(std::max_align_t) char buffer[1024];

    // 2. Initialize the allocator to use this buffer
    shm::linear_allocator<MyTag> arena(buffer, sizeof(buffer));

    // 3. Create the STL-compatible allocator instance
    using ShmStlAlloc = shm::linear_allocator<MyTag>::stl_allocator<char>;
    ShmStlAlloc stl_alloc(arena);

    // 4. Create and use STL containers
    // The vector's memory will come from our lock-free arena.
    std::vector<int, shm::linear_allocator<MyTag>::stl_allocator<int>> my_vec(stl_alloc);
    my_vec.push_back(10);
    my_vec.push_back(20);

    // Strings work the same way
    using ShmString = std::basic_string<char, std::char_traits<char>, ShmStlAlloc>;
    ShmString my_str(stl_alloc);
    my_str = "hello, shared memory";

    std::cout << "Vector contents: " << my_vec[0] << ", " << my_vec[1] << std::endl;
    std::cout << "String contents: " << my_str << std::endl;
    std::cout << "Total memory used in arena: " << arena.used() << " bytes" << std::endl;

    return 0;
}
````

When you use arena-backed standard containers, the correct posture is to treat the container as an arena-scoped consumer. Container destruction does not reclaim arena bytes. `clear()` does not reclaim arena bytes. Shrinking does not reclaim arena bytes. If a vector grows through multiple reallocations, each discarded backing buffer remains consumed. If you do not size containers with this in mind, you will over-consume the arena and then observe allocation failure earlier than expected.

The STL adapter signals allocation failure by throwing `std::bad_alloc`. This is the standard-library contract. If exceptions are not acceptable at the call site, do not use the STL adapter in that code path. Use the `alloc` or `allocate` primitives and propagate null explicitly.

## Practical segment layout and handle discipline

A segment that uses `linear_allocator` typically has a fixed header that stores segment metadata and one or more root handles into arena-allocated structures. The allocator is then constructed with `segment_base` equal to the mapped base and `arena_start` equal to the first byte after the header. Handles stored in the header are segment-relative and are decoded by any process that maps the segment and establishes the base for the segment tag.

A mechanically correct design stores only segment-relative handles inside the shared bytes. Raw pointers returned by `alloc` and `allocate` are permitted only as transient local variables within a process. If you write raw pointers into the segment, you have violated the position-independent representation model, regardless of whether the pointers “happen to work” in one process.

If you allocate and construct an object and then store a handle to it into shared state that other participants may read, you must define a publication protocol. Allocation success means only that the storage is reserved. It does not imply that the object is visible as fully initialized to other observers. The allocator does not provide this protocol because it must be consistent with your graph invariants and mutation policy.
