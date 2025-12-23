# Allocators: `linear_allocator` Arena Allocation for Position-Independent Data

This library provides `linear_allocator<Tag, OffsetT>`, a thread-safe linear allocator intended to allocate storage inside a contiguous byte region that you treat as an arena. The allocator is intentionally not a general-purpose heap. It is an arena allocator with a monotonic “bump” cursor. Individual deallocation does not exist. The only reclamation operation is resetting the entire arena back to empty.

This document is a practical guide. It describes how to wire the allocator into a segment, how to allocate raw bytes and typed objects, how to obtain position-independent handles (`segment_offset_ptr`) suitable for storing into the shared bytes, how to integrate the allocator with the standard library’s allocator model, and how to reason about correctness under concurrency.

## The Arena Model

The library provides a thread-safe linear (arena) allocator. Allocations are fast atomic pointer bumps. Deallocation is a no-op. The entire arena is cleared at once via `reset()`. This model is designed for high-throughput, frame-based, or batch-processing IPC workloads where object lifetime is tied to the arena, not individual items.

What this means in concrete terms is that every successful allocation permanently consumes some prefix of the arena until you reset. If you allocate a node, then decide you do not want it, the bytes backing that node remain consumed. If a container allocates a larger backing buffer during growth, then later shrinks or is destroyed, the allocator does not reclaim those bytes. The only way memory becomes available again is that you stop using the arena’s contents as a whole and call `reset()` to rewind the cursor to zero.

This model is not “an optimization.” It is a lifetime policy. It trades per-allocation free, fragmentation management, and complex metadata for a single atomic counter and a single global lifetime boundary. In workloads where you naturally have a boundary between batches, frames, transactions, epochs, or messages, the boundary is the place you reset. If your workload does not have such boundaries, a linear arena is the wrong allocator, because it will eventually fill.

The allocator is designed to work with the position-independent representation model of this project. It is common to allocate objects inside a mapped segment and then store `segment_offset_ptr` handles to those objects in shared structures. In that regime, the allocator is not just an implementation detail; it is part of the shape of the segment. It determines the lifetime of everything allocated from it.

## Construction, Segment Base, and What Actually Lives in Shared Memory

`linear_allocator<Tag, OffsetT>` is parameterized by a `Tag` and an `OffsetT`. The `Tag` ties the allocator to the segment anchoring system. The allocator’s “handle” type is a segment-relative `offset_ptr` anchored by `segment_anchor<Tag>`. That means the allocator can return references that are stable under relocation of the segment as a whole, as long as each process establishes the segment base for `Tag`.

The allocator constructor calls `shm::segment_base<Tag>::set(segment_base)` as a side effect. This matters because it establishes the base used to decode any `segment_offset_ptr<..., Tag, ...>` in the current process. If you have multiple segment mappings in one process that share the same `Tag`, you cannot use them simultaneously without an explicit policy for switching the base. The `Tag` is the namespace boundary. Treat it as a real namespace, not as decoration.

There are two construction forms.

`linear_allocator(void* start, std::size_t size)` constructs an arena that begins at `start` and spans `size` bytes. It also sets the segment base for `Tag` to `start`. This is appropriate when the arena begins at the mapped segment base and you want handles to be relative to that same base.

`linear_allocator(void* segment_base, void* arena_start, std::size_t arena_size)` constructs an arena whose allocations are carved from `[arena_start, arena_start + arena_size)`, but whose handles are relative to `segment_base`. This form is the one you use when the segment has a header or multiple subregions. In that shape, the mapped segment base is fixed by the OS mapping, and the arena might begin after a header, after a directory, or inside a partition. You still want every handle to be expressed in the same coordinate system, and that coordinate system is the segment base.

In all cases, the allocator returns raw pointers for immediate use and also supports returning segment-relative handles for storage into shared bytes. A raw pointer returned by the allocator is a process-local virtual address and is not a persistent representation. A handle returned by the allocator is a position-independent reference that is intended to be stored into the segment and later decoded by other processes after establishing the base.

The allocator object itself is a C++ object with members that include process-local pointers. It is not a “position-independent blob type” you can memcpy into a file and expect to work elsewhere. The arena bytes it manages can be position-independent; the allocator façade is not automatically position-independent unless you deliberately place the allocator control block in shared memory and ensure that every process interprets it correctly. If you need a cross-process allocator control block, the mechanically relevant requirement is that the cursor state that determines the next allocation offset must be the same shared atomic location for all participants.

## API Reference

This section enumerates the core operations you use in normal code. The details under each name are the semantics you rely on. Where the semantics are “no policy,” that is intentional and you must supply the policy above this layer.

### `void* alloc(std::size_t n, std::size_t alignment = alignof(std::max_align_t)) noexcept`

`alloc` is the primitive operation. It reserves `n` bytes from the arena and returns a pointer to the beginning of an aligned region of storage. The allocation is performed by atomically advancing an internal cursor. If the arena does not have enough remaining space, `alloc` returns `nullptr` and consumes nothing.

If `n` is zero, `alloc` returns `nullptr`. This is a deliberate choice to keep the API honest: allocating zero bytes does not create a usable storage region, and treating it as success invites code that later assumes it can write.

If `alignment` is zero, it is treated as `1`. Power-of-two alignments are handled with mask-and-round-up arithmetic. Non-power-of-two alignments are accepted and handled by modular arithmetic. The returned pointer is aligned to the requested alignment, assuming the arena itself is properly aligned and the internal arithmetic does not overflow. Alignment can introduce padding gaps between allocations. Those gaps are not reusable until reset because the allocator is monotonic.

The allocation either succeeds and returns a pointer inside the arena, or fails and returns `nullptr`. It never throws.

### `T* allocate<T>(std::size_t count = 1) noexcept`

`allocate<T>(count)` is typed allocation. It reserves storage for `count` objects of type `T` and returns a `T*` aligned to `alignof(T)`. It does not construct objects. It only allocates storage.

If `count` is zero, it returns `nullptr`. If `count * sizeof(T)` overflows `std::size_t`, it returns `nullptr`. If the arena cannot satisfy the allocation, it returns `nullptr`.

The storage returned by `allocate<T>` is raw. If you are using resident types that require construction to begin lifetime, you must use placement new into that storage before treating it as a live `T`. If you are using implicit-lifetime resident types and your policy permits interpreting zeroed or copied bytes as objects, the allocator does not enforce or validate that policy. It only provides storage.

### `handle<T> make_handle<T>(Args&&... args)`

`make_handle<T>(args...)` allocates storage for a single `T`, constructs `T` in place with placement new, and returns a segment-relative handle of type `handle<T>`, where `handle<T>` is an alias for `shm::segment_offset_ptr<T, Tag, OffsetT>`.

If the allocation fails, `make_handle` returns a null handle, meaning a handle whose encoded representation is the null encoding and whose `get()` decodes to `nullptr` in the current process.

If `T`’s constructor is nothrow, `make_handle` is noexcept. If `T`’s constructor can throw, `make_handle` will propagate the exception and the reserved bytes remain consumed in the arena. There is no rollback. This is consistent with the arena model: partial construction can leak arena space until the next reset. If that is unacceptable, do not construct throwing types in the arena.

`make_handle` does not register destructors. When you reset the arena, destructors are not executed. If `T` has a non-trivial destructor whose effects matter, then either you must manually call the destructor before reset or you must not allocate such types from the arena. In the project’s shared-memory posture, the usual rule is that resident types do not own external resources and their destructors are either trivial or semantically irrelevant for correctness.

### `void reset() noexcept`

`reset()` sets the arena cursor back to zero. After `reset()`, the allocator will begin allocating from the beginning of the arena again. Any previously returned raw pointers and any previously returned handles become stale in the sense that their storage may be overwritten by subsequent allocations. The allocator does not poison memory and does not invalidate handles in-band. It simply reuses bytes.

`reset()` does not call destructors for objects constructed with `make_handle` or constructed manually in storage returned by `alloc` or `allocate`. If you constructed non-trivial objects whose destructors must run for correctness, that is your responsibility, and in shared-memory resident designs the usual policy is to avoid such types entirely.

`reset()` is intentionally constant-time regardless of how many allocations have been performed, because it does not traverse metadata. It writes a single atomic cursor.

### Additional operations you will use in real code

The allocator provides `alloc_handle(n, alignment)` and `allocate_handle<T>(count)`, which are the handle-returning analogs of `alloc` and `allocate`. They return segment-relative handles instead of raw pointers. Use the handle-returning forms when the reference needs to be stored in shared bytes.

The allocator provides `used()` and `capacity()` for instrumentation and capacity planning. `used()` returns the current cursor value in bytes, which includes any alignment padding consumed by allocations. It is not “live bytes of objects.” It is “bytes reserved from the arena since last reset.”

The allocator provides `owns(p)` as a membership check that reports whether a raw pointer lies within the arena address range in the current process. This is useful for debug assertions and sanity checks, not as a security boundary.

The allocator provides `secure_reset()`, which zeroes the used prefix of the arena bytes and then resets the cursor. This is intended for workflows where you want to clear the arena contents between batches to avoid retaining sensitive data or to reduce the chance of consumers reading stale bytes. It is a byte wipe, not a durability protocol and not a cryptographic erasure guarantee.

## Thread Safety Guarantee

All allocation functions are lock-free and thread-safe. Multiple threads or processes can allocate from the same `linear_allocator` instance without external locking. The `reset()` function is not thread-safe and must be called only when no other threads are accessing the allocator.

The mechanically relevant reason that allocation is thread-safe is that all allocation operations linearize at a single atomic cursor update. Each successful allocation reserves a disjoint byte range by performing a compare-and-exchange that advances the cursor from an observed value to a larger value that accounts for alignment and size. If two threads race, one wins and the other retries with a new cursor value. The allocator never returns overlapping ranges for successful allocations because overlap would require two successful CAS operations to commit the same cursor transition, which cannot happen.

The mechanically relevant reason that `reset()` is not thread-safe is that it rewinds the cursor without coordinating with active allocations and without coordinating with active users of the already-allocated storage. If a thread allocates while another thread resets, you can produce two allocations that overlap, because the reset can cause the cursor to move backward while another thread is still executing with an older view of the cursor. If a thread reads or writes an object while another thread resets and reallocates over it, you have created a use-after-recycle bug at the arena level. The allocator does not attempt to prevent this. It is a boundary you must enforce.

When you use the allocator in a shared-memory or IPC design, the “thread safety” statement must be read as “the cursor update is safe under concurrency.” It does not imply that the objects you build in the arena are safe under concurrency, and it does not imply that object publication is safe without a protocol. Allocation reserves bytes. It does not initialize bytes. It does not provide release-acquire publication semantics for the contents you write into those bytes. If a consumer in another thread or another process must observe a fully initialized object, you must publish the handle with a release operation and read it with an acquire operation, or use locks, or use an equivalent correctness protocol. The allocator does not invent that protocol for you.

## Alignment, Padding, and Capacity Planning

The allocator supports arbitrary alignments, but alignments consume padding. The cursor measures reserved space, not just payload. If you allocate many small objects with large alignment, the used count can grow much faster than the payload bytes. This is not a bug. It is the unavoidable effect of aligning each allocation independently inside a monotonic arena.

You should treat `capacity()` as a hard byte budget and treat `used()` as the monotonic consumption against that budget. If allocations fail, `alloc` returns `nullptr` and the STL adapter throws `std::bad_alloc`. In both cases, the failure means the arena has no remaining space for that allocation shape at that moment.

If you are building container-heavy structures in the arena, reserve capacity aggressively to reduce reallocation churn. Reallocation churn is not just “slower.” In an arena allocator, reallocation churn permanently consumes extra buffers that would have been freed in a general-purpose heap. A vector that grows by repeated doubling consumes multiple backing buffers over time, and every discarded buffer remains consumed until reset. If you know the approximate size of the container in a batch, call `reserve` early, because that has a direct effect on peak arena usage.

## Handles, `Tag`, and Position-Independent References

The allocator defines `handle<T>` as a segment-relative `offset_ptr` anchored by `segment_anchor<Tag>`. This is the allocation-to-representation bridge.

If you allocate an object and want to store a reference to it inside the shared segment, do not store the raw pointer. Store the handle. A raw `T*` is meaningful only in the allocating process and only while the mapping remains at the same virtual address. A handle is meaningful across processes as long as each process establishes the same segment base for `Tag`.

The allocator constructor sets `segment_base<Tag>` for the current process. That means that if you construct the allocator as part of segment mapping initialization, you have established the base necessary for decoding handles. If you decode handles without establishing the base, you are in error territory. In debug builds, this may assert. In release builds, it may decode garbage.

If you have a segment layout that contains multiple arenas, you typically pick one `Tag` per segment and then construct each arena with the same `segment_base` and different `arena_start` values. The base defines the coordinate system. The arena start defines where you allocate from. Handles must remain in the base coordinate system, not in “arena-local” coordinates, because you want a single consistent interpretation across the segment.

## STL Integration (Usage Example)

This section shows a copy-pasteable wiring of `linear_allocator` into standard containers. The critical constraint is that deallocation is a no-op. These containers will behave correctly as containers, but their memory behavior will match the arena model: allocations consume arena space monotonically, and memory is reclaimed only when you reset the arena.

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

This example uses a stack buffer to demonstrate the mechanics in a single process. In a real segment, `buffer` would be a pointer into a mapped shared memory region, and you would choose the arena start and size according to your segment layout. The allocator still behaves the same way because it only requires a contiguous byte range.

When you use STL containers with the arena allocator, you must internalize one non-negotiable fact: container destruction does not return memory to the arena. `my_vec` going out of scope does not make arena space available. `my_str` shrinking does not make arena space available. That is not a container bug. It is the allocator policy. The way you reclaim memory is to end the batch that owns the arena contents and call `reset()` on the arena, at a time when no live references to arena-allocated objects remain.

## What `stl_allocator<T>` Actually Guarantees

`linear_allocator<Tag, OffsetT>::stl_allocator<T>` is an STL-compatible allocator adapter that forwards allocations to the arena and throws `std::bad_alloc` on failure. It stores a pointer to the arena instance. Allocator propagation traits are set to propagate on copy assignment, move assignment, and swap. Equality compares the underlying arena pointer, because two allocators are equal only if they allocate from the same arena.

`deallocate` is a no-op by design. That means the adapter satisfies the allocator interface but does not provide reclamation. Standard containers are permitted to call `deallocate` frequently as part of their internal algorithms. Those calls do nothing, which is consistent with the arena model but can surprise users who assume that “free” happens at container boundaries.

If you pass this allocator to containers whose algorithms rely on releasing memory to stay within a budget, you will consume arena space faster than expected. The correct posture is to treat arena-backed containers as batch-scoped and to design the batch boundaries explicitly.

## Failure Modes and How to Handle Them Without Lying to Yourself

Allocation failure is not exceptional in the arena model. It is the normal signal that your batch exceeded its capacity budget. The low-level API signals failure by returning `nullptr`. The STL adapter signals failure by throwing `std::bad_alloc`. Both signals mean the same thing: the arena cannot satisfy the request, and there is no fallback inside this allocator.

If allocation failure is possible in your workload, you must decide a policy at the batch boundary. The usual policies are to size the arena so failure does not happen in normal operation, to split the workload into smaller batches, to detect oversize inputs and reject them early, or to implement an overflow path that allocates from a different allocator and marks those objects as non-segment-resident. The arena allocator does not pick a policy for you.

If you do not want exceptions, do not use the STL adapter in code paths where you cannot tolerate `std::bad_alloc`. Use the `alloc` and `allocate` primitives and propagate null up to the boundary where you can handle it.

## Publication and IPC Reality

The allocator can be used to reserve storage in shared memory. That is not enough to make a correct IPC data structure.

If one thread or process allocates storage, constructs an object in that storage, and then writes a handle to that object into a shared location that other threads or processes read, you must define a publication protocol. The common publication protocol is that the writer performs all initialization, then stores the handle with release semantics, and readers load the handle with acquire semantics before dereferencing it. Without this, readers can observe partially initialized objects even if the handle itself is valid and even if the allocator is perfectly thread-safe.

The allocator does not attempt to enforce this because publication correctness depends on your graph invariants, your mutation policy, and your durability requirements. The allocator is only the storage reservation mechanism.


## Practical Integration Patterns

If you are building a single segment that contains a header followed by arena-allocated objects, construct the allocator with `segment_base` equal to the mapped base and `arena_start` equal to the first byte after the header. This keeps the handle coordinate system stable and ensures that all handles remain valid regardless of where the arena begins.

If you want multiple arenas inside one segment, use a single `Tag` for the segment and carve disjoint subranges for each arena. Each arena gets its own cursor and therefore its own lifetime boundary. Every arena still returns handles in the same coordinate system because the base is the same segment base. This keeps cross-arena references representable and keeps decoding uniform.

If you want to allocate transient scratch objects that never need to be referenced by handles, use the raw pointer-returning API and do not store those pointers into shared bytes. A raw pointer is allowed as an ephemeral local variable. It is not an allowed representation.

If you need to reclaim memory at finer granularity than whole-arena reset, this allocator is not the right tool. Do not attempt to simulate frees by manually rewinding the cursor unless you can prove a strict LIFO discipline and can prove no aliasing references exist. The allocator does not provide or enforce that discipline.


