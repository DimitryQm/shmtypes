# `offset_ptr` API and Mechanics

`offset_ptr` is a relocatable reference type for position-independent data stored inside a contiguous region such as shared memory or a memory-mapped file. It stores an integral displacement rather than a process-local address, and it reconstructs a usable `T*` on demand by adding that displacement to an anchor-defined base.

The intent of this document is to specify the user-visible API surface and the semantics you can rely on. It is not a tutorial and it does not attempt to soften the constraints that fall out of the model.

## Type and Parameters

The primary type is `offset_ptr<T, Anchor, OffsetT>`.

`T` is the pointed-to type. The library does not make arbitrary `T` safe for shared memory; `T` should be chosen according to the project’s shared-memory safety rules. In most production designs, `T` is standard-layout and trivially copyable, and it does not contain raw pointers, references, vtables, or allocator-owning standard library types.

`Anchor` defines the coordinate system in which the stored displacement is interpreted. The anchor is the semantic lever that determines when relocation is guaranteed to work and when it is not. The two canonical anchors are `self_anchor` and `segment_anchor<Tag>`.

`OffsetT` is the stored integer type that encodes the displacement. This choice is a storage and performance trade. A 32-bit offset is typically preferable when the segment is smaller than 4 GiB because it shrinks node size and improves cache density. A 64-bit offset removes practical size limits at the cost of larger nodes and higher memory bandwidth usage. Signed offsets permit backward references; unsigned offsets constrain references to be forward from the chosen base. The library treats `OffsetT` as the representation type, and if you persist blobs long-term you should freeze this choice as part of your on-disk or on-wire schema.

## Anchor Semantics

### `self_anchor`

With the self anchor, the displacement is interpreted relative to the address of the `offset_ptr` field itself. The base used to decode is the pointer object’s own address. This mode requires no global initialization. It is attractive for small, locally-constructed graphs that are treated as a single relocatable blob.

Self-relative anchoring is not a general-purpose replacement for raw pointers in container-heavy segments. Its key limitation is that the meaning of the stored displacement depends on where the `offset_ptr` field resides. If a container relocates the object containing the `offset_ptr` without also preserving the relative position between that field and its pointee, the stored displacement no longer reaches the intended target. In other words, self-relative pointers are stable under whole-blob relocation but not stable under independent relocation of subobjects.

In implementations that rebase on copy, copying a self-relative `offset_ptr` is not a bitwise operation. The copy constructor and copy assignment resolve the source to a raw pointer and then re-encode the displacement relative to the destination object. This is required for correctness under ordinary C++ copying semantics, and it means the type is not trivially copyable in this mode.

### `segment_anchor<Tag>`

With the segment anchor, the displacement is interpreted relative to a process-local base pointer representing the start of the mapped region. The base is provided by `segment_base<Tag>::set(mapped_base)` and later retrieved via `segment_base<Tag>::get()`.

Segment-relative anchoring is the production default for shared memory that contains multiple allocators, containers, or any pattern where objects may move within the segment. The meaning of a segment-relative displacement does not depend on where the `offset_ptr` field resides. It depends only on the segment base. This decoupling is what makes it robust under container relocation and internal movement within the segment as long as the relative layout inside the segment is preserved.

In implementations that specialize for segment anchoring, copying and moving are intended to be bitwise operations on the stored integer. That property is a performance feature and a correctness feature for container usage. You should still treat the segment base as mandatory initialization, because decoding a segment-relative offset without a valid base is an error in program logic.

### `segment_base<Tag>` initialization

When using `segment_anchor<Tag>`, each process that maps the region must set the base pointer before dereferencing any segment-relative `offset_ptr` stored in that region. The base pointer is not stored in the shared bytes; it is process-local state.

If your application spans multiple shared libraries on Windows, be aware that header-only inline statics may be duplicated per module. If the segment base is stored as an inline static inside a header and you call `segment_base<Tag>::set()` in one DLL but decode in another DLL, you can end up with inconsistent bases. In such deployments, centralize the base storage in a single module or provide an exported setter/getter so every consumer resolves the same base pointer.

## Null Representation and the Self-Reference Trade

`offset_ptr` reserves the stored value `0` to represent `nullptr`. This is done to keep the representation compact and to make null checks a simple integer comparison.

The common encoding is “offset plus one.” The stored integer is `offset + 1` and a stored value of `0` means null. On decode, the effective offset is `stored - 1`. This scheme implies a specific trade: an offset of zero is unavailable because it collides with null. In practical terms, a pointer cannot point to the anchor base with an offset of zero. In self-anchored mode this means the pointer cannot point to itself, because that would require an offset of zero. This is an intentional trade for space efficiency and fast null checks.

If you need to represent self-references or base-pointing references, represent them explicitly at a higher level. Do not assume `offset_ptr` can express every degenerate pointer pattern.

## API Surface

The following members are the intended stable surface. The library may provide additional members for diagnostics or integration, but you should treat the items below as the contract.

### Construction

A default-constructed `offset_ptr` represents null.

Construction from `nullptr` produces null.

Construction from `T*` encodes a displacement from the anchor base to the target pointer. Passing `nullptr` produces null. Passing a pointer not within the addressable domain of the anchor is a logic error. For segment anchoring, “addressable domain” means within the mapped segment. For self anchoring, it means within the same relocatable blob with the invariants you have chosen. This library does not attempt to validate membership in release builds.

Copy construction and copy assignment preserve the logical referent. In self-anchored mode this generally requires rebasing, meaning the stored displacement is recomputed for the destination object. In segment-anchored mode this is typically a bitwise copy of the stored integer.

Converting construction from `offset_ptr<U, Anchor, OffsetT>` is permitted only when `U*` is convertible to `T*`. The conversion preserves the referent and uses the same anchor. This is intended for const propagation and for conversions between related types where the pointer conversion is valid.

### `get()`

`T* get() const noexcept` returns a raw pointer reconstructed from the stored displacement and the anchor base. If the stored value represents null, `get()` returns `nullptr`. The returned pointer is meaningful only within the current process mapping and only while the underlying segment is still mapped. It is not stable to store the returned raw pointer back into shared memory.

`get()` is a pure decoding operation. It does not synchronize with other threads and it does not validate that the target address holds a live `T` object. It assumes the segment is internally consistent according to your construction rules.

### Dereference and Member Access

`T& operator*() const` dereferences `get()`. Calling it when null is undefined behavior in the same way dereferencing a null raw pointer is undefined behavior.

`T* operator->() const` returns `get()` and is only meaningful for non-void `T`.

If the implementation provides `operator[]`, it is only meaningful when the stored referent points into an array-like sequence of `T` objects. The library does not impose array semantics; it merely forwards to pointer arithmetic on the decoded raw pointer.

### Boolean and Comparisons

`explicit operator bool() const noexcept` is a null check. It evaluates to false when the stored value is the null representation.

Equality comparisons, if provided, compare referents after decoding, not raw stored offsets, because two different offsets could theoretically decode to the same address under different base interpretations. For most practical designs within a single segment and a fixed base, comparing stored values is equivalent, but the semantic contract is in terms of referents.

### `raw_storage()`

`OffsetT raw_storage() const noexcept` exposes the stored integer encoding. This is primarily for debugging, instrumentation, and introspection. It is not a promise of stable on-disk format unless your project explicitly freezes it. If you persist blobs, treat `OffsetT` and the encoding scheme as part of your schema and version it accordingly.

`raw_storage()` is also useful for fast equality checks in segment-relative mode in tightly-controlled code, but such usage should be justified by profiling and should not replace correctness reasoning.

## Thread Safety and Atomicity

Reads are const-correct. Writes are not atomic.

Calling `get()` concurrently from multiple threads is safe only if no thread writes to the same `offset_ptr` concurrently. Updating an `offset_ptr` from multiple threads or reading while another thread writes without synchronization is a data race and therefore undefined behavior.

If you need concurrent publication of pointers, wrap the stored representation in an atomic and define a publication protocol with release and acquire semantics. The library does not implicitly make any operation atomic and does not insert memory fences.

Across processes, the same rules apply. The fact that memory is shared does not relax the C++ data race rules; it simply makes violations harder to debug.

## Range, Overflow, and Segment Limits

`OffsetT` bounds the representable displacement range. If the computed displacement does not fit in `OffsetT`, encoding overflows. Production builds typically treat this as undefined behavior or as a hard invariant violation. Debug builds may assert.

Choose `OffsetT` so that your maximum segment size and your maximum addressable span from the chosen base fit comfortably. When using unsigned offsets, also ensure your design does not require negative displacements. When using signed offsets, ensure you understand the implications for range and for how you partition the segment.

If your segment can exceed 4 GiB, a 32-bit offset is insufficient. Do not attempt to “make it work” with partial bases or ad hoc rebasing without explicitly designing that policy; choose a 64-bit offset and accept the footprint.

## Alignment and Object Lifetime

`offset_ptr` does not enforce alignment of the pointee. It assumes that the address it decodes points to a properly aligned `T`. If you violate alignment, you have undefined behavior regardless of how correct the displacement arithmetic is.

`offset_ptr` also does not create object lifetimes. If you interpret arbitrary bytes as a `T` without constructing a `T` there in a manner consistent with the C++ object model, you are in undefined behavior territory. Many shared-memory systems intentionally restrict resident types to implicit-lifetime types and treat the segment as a serialized representation that is “object-like” by construction. If you are taking that approach, do it deliberately and document it in your schema. If you are not, then you must construct objects in-place using placement new and manage lifetimes explicitly.

These are not theoretical concerns. A pointer encoding that relocates correctly is still wrong if it points into storage that does not contain a live object.

## Error Handling Philosophy

Decoding a null is well-defined. Decoding a non-null that was never valid is not.

The library does not attempt to be defensive by default. In release configurations, decoding is a small integer operation plus an addition. The expectation is that you validate structure integrity at a higher layer, either through segment headers, invariants, checksums, or application-level consistency checks. Debug-only assertions may exist to catch obvious misuse, but correctness in production relies on respecting the memory model and invariants.

## Integration Notes

Use segment anchoring when you intend to place `offset_ptr` inside relocatable containers or when your segment contains multiple independently-moving substructures.

Use self anchoring when you can guarantee whole-blob relocation with stable internal layout and you want zero global initialization.

Do not persist raw pointers returned by `get()`.

Do not store polymorphic objects in the segment.

Do not update `offset_ptr` concurrently without synchronization.

