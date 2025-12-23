# Memory Model: Position-Independent Shared Data

This library targets the class of systems where “the data is the file” or “the data is the segment”: a contiguous byte region that may be shared between processes, mapped from disk, snapshotted, sent over a socket, or otherwise moved around as raw bytes. The design goal is not to make ordinary heap objects magically shareable. The design goal is to make shared data mechanically correct under relocation by construction.

The core technique is to stop encoding relationships as process-local virtual addresses and to encode them instead as offsets within a region. If your representation never depends on where the region happened to land in a particular process, then relocation becomes a property of the representation rather than an expensive fixup step.

## The Two-Universe Problem

You must keep two coordinate systems separate.

In the first universe, a pointer value is a virtual address in a particular process. A raw pointer like `T*` is a coordinate in that process’s private address space. That coordinate is produced under assumptions that are not stable across processes. Address Space Layout Randomization, different shared library load addresses, different allocator states, and different stack positions all ensure that two processes mapping the same underlying bytes will not agree on addresses.

In the second universe, an offset is a displacement within a byte region. An offset does not name an absolute address; it names a position relative to a base. If two processes map the same region, the base address in each process may differ, but the displacement from base to a specific object within the region is invariant as long as the relative layout of bytes inside the region is preserved.

Shared-memory bugs happen when the first universe leaks into the second. Storing raw pointers inside a shared blob embeds process-private coordinates into shared state. When a second process reads those bytes, it interprets the embedded coordinates inside its own address space and either crashes immediately or, worse, reads and writes unrelated memory. Position-independent data is the deliberate decision to represent relationships in the offset universe and to treat raw addresses as ephemeral implementation details of a mapping.

## The Relocation Guarantee

Data structures built with this library are Position Independent (PIC). They can be memcpy'd to disk, network, or other processes without serialization.

This guarantee is about eliminating pointer fixups. If your object graph is composed of position-independent fields and references, then the blob itself is the representation. You can copy the bytes as a unit and the references remain valid in the destination mapping as soon as the decoding base is established. There is no traversal step whose job is “rewrite every pointer,” because there are no absolute pointers to rewrite.

The guarantee is not magic; it is a contract. The contract is satisfied when your shared-memory resident types do not embed process-local addresses and do not embed process-local runtime state, when the relative layout of the bytes inside the region is preserved, and when the consumer interprets the bytes with a compatible schema. Position independence solves relocation. It does not solve schema evolution, ABI drift, endianness differences, or corruption detection. Those are separate policies you must define and enforce at the segment boundary.

## What This Model Does Not Promise

Position independence does not imply that arbitrary C++ types are safe to place into shared memory. It does not imply that you can ignore padding, packing, alignment, or the C++ object lifetime model. It does not imply that a blob written by one build of a program is consumable by another build if the struct layout changed. It does not imply that bytes can be transported across heterogeneous machines without an explicit compatibility policy for endianness and layout.

Treat “PIC” as “location independent,” not “universally interoperable.”

## Shared-Memory Resident Types and the Boring-Type Doctrine

The simplest production posture is to restrict shared-memory resident types to representations whose meaning is their bytes. In practice that means types that are standard-layout and trivially copyable, or at least types that behave like implicit-lifetime objects when placed into raw storage. This restriction is not about dogma. It is about making your segment representation predictable and inspectable, and about ensuring that relocation by raw byte copy has the semantics you think it has.

Ordinary heap objects are usually the opposite of this. They are full of hidden pointers and hidden ownership: allocator metadata, internal node pointers, small-string buffers with embedded addresses, virtual dispatch, and other process-local details. If you place those representations into a shared segment, you have exported the producer’s address universe and runtime state into a context where the consumer cannot interpret it safely.

If you want collections inside a segment, build them out of fixed-layout nodes and offset-based references, or use containers explicitly designed for position-independent storage where all internal links are offsets and all allocation is performed inside the segment using a segment-aware allocator.

## The VTable Prohibition

This is a hard rule.

Users cannot put classes with virtual functions inside shared memory.

The reason is not stylistic; it is mechanical. A polymorphic C++ object carries an implementation-defined hidden field commonly called the vptr. That vptr points to a vtable located in the program’s code and read-only data segments. Those segments are mapped per process and are not shared as a stable address range across independent processes. Even when two processes run the “same” executable, the loader’s decisions and ASLR mean that the vtable address in Process A is not the vtable address in Process B.

If Process B attempts to call a virtual function on an object created by Process A, Process B will crash. The object’s vptr still points at Process A’s code-space coordinate, and dereferencing it in Process B is an invalid jump target. Best case is an immediate fault. Worst case is control-flow into unintended code if the address happens to be mapped.

The prohibition extends naturally to other forms of code-address embedding. Function pointers, member-function pointers, and callback objects that capture code addresses are not position-independent data. If you need dynamic behavior, represent it explicitly as data that can be interpreted by process-local code, such as an enum tag plus a payload, or a compact command/message protocol that is executed outside the segment.

## The Alignment Rule

Offset-based references do not repair incorrect alignment. Every object inside the segment must still satisfy C++ alignment requirements.

If you place an object of type `T` at an address that is not a multiple of `alignof(T)`, the program has undefined behavior. Some architectures trap on misaligned accesses, some silently perform slower fixups, and some produce incorrect results. A mapped region may start at an address that feels “weird,” but that is not a problem as long as you enforce alignment for each object within the region.

In practice, treat the segment as an arena. When you carve storage from it, you must align allocations. When you define shared-memory layouts, you must use `alignas` where needed. When you use placement new into a raw buffer, you must ensure the pointer you are constructing into is properly aligned for the type you are constructing. Debug builds should assert alignment aggressively, because alignment bugs tend to be platform-dependent and expensive to diagnose after the fact.

## Relocation Granularity and Anchor Semantics

An offset is only meaningful relative to an anchor, and the anchor choice is part of the memory model.

A self-relative reference interprets its offset relative to the address of the `offset_ptr` field itself. This requires no global initialization and works well when the entire blob is treated as a single relocatable unit whose internal layout never changes after construction. It is not appropriate when pointer fields and pointees can move independently, which is common in container growth and reallocation patterns. In those cases, a self-relative reference can become invalid even though the region as a whole is still “the same data.”

A segment-relative reference interprets its offset relative to a segment base. This requires that the base be established per process after mapping, but it makes references invariant under internal relocation within the segment as long as the relative layout of objects inside the segment is preserved. For shared memory that contains multiple allocators, multiple subregions, or container churn, segment-relative anchoring is the production default because it decouples reference meaning from the pointer field’s own address.

This is a semantic choice first and an optimization choice second. If you want strong relocation behavior under container movement, you should treat segment-relative anchors as the normal mode and reserve self-relative anchors for cases where you can prove the invariants that make them safe.

## Concurrency, Publication, and the C++ Memory Model

This library makes references relocatable. It does not make your segment automatically safe under concurrent mutation.

An `offset_ptr` is represented as an integer. Reading it is a normal integer load. Writing it is a normal integer store. That means the usual C++ memory model applies: unsynchronized concurrent reads and writes to the same location are data races and therefore undefined behavior. The fact that the memory is shared between processes does not change the rules; it only makes the consequences harder to reproduce.

If you need concurrent mutation, you must supply synchronization or a publication protocol. A publication protocol typically means that an object is fully initialized first, then a reference to it is stored with release semantics, and readers load that reference with acquire semantics before accessing the object. Lock-based designs are often simpler and more robust in shared-memory systems, especially when multiple processes are involved and failure modes must be understood.

The segment representation should make your concurrency policy explicit. If readers can observe partially written state, you have built a nondeterministic system regardless of how elegant the pointer encoding is.

## Durability and Crash Consistency

Relocation correctness is not crash consistency.

If the segment is file-backed or must survive process crashes, you need a durability protocol. Without one, you can persist partially written graphs where offsets point to uninitialized or torn state. Position-independent data makes it easier to persist because you can copy bytes directly, but it also makes it easier to persist corruption if you do not define atomic update boundaries.

Production designs typically include a small segment header with a magic value, versioning, and a consistency marker, along with a strategy for safe updates such as double-buffering, journaling, or copy-on-write snapshots. Those policies sit above the pointer model, but you should design them alongside it because they constrain how and when shared objects may be mutated.

## Cross-Platform Considerations

The position-independent representation is platform-agnostic. The mapping mechanism is platform-specific.

On POSIX systems, the mapping layer is usually `mmap` and related APIs. On Windows, it is file mapping objects and view mappings. In all cases, the base address of the mapping is a per-process detail and therefore must not be embedded as an absolute pointer inside the blob. If you use a segment-relative anchor, each process must establish the base after mapping.

When your program spans multiple dynamically loaded modules on Windows, be aware that a header-only global base stored as an inline static may be duplicated per module. In such a deployment, centralize the base storage in a single module or provide an exported setter and getter so every consumer resolves the same base pointer.

## Summary

The memory model is blunt because it must be. Virtual addresses are private to a process. A shared segment is a shared byte region. If you store private addresses in shared bytes, you have built a representation that cannot survive relocation. A position-independent representation stores relationships as offsets relative to an agreed anchor and restricts resident types to layouts whose meaning is stable as bytes. The payoff is the relocation guarantee: you can copy the blob as bytes and traverse it in the destination without pointer fixups. The constraints are equally real: vtables are forbidden, alignment must be respected, concurrency requires explicit synchronization, and long-lived persistence requires a durability protocol.
