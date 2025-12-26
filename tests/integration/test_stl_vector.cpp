#include "shmTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace {

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            std::cerr << "CHECK failed: " #expr " @ " << __FILE__ << ":" << __LINE__ << "\n";      \
            std::abort();                                                                           \
        }                                                                                           \
    } while (0)

static inline std::uint32_t get_pid_u32() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

static std::string make_unique_seg_name() {
    std::string s;
    s.reserve(64);
    s.append("/shm_stl_vector_");
    s.append(std::to_string(get_pid_u32()));
    return s;
}

static inline std::uintptr_t uaddr(const void* p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

static inline bool in_range(std::uintptr_t p, std::uintptr_t base, std::size_t len) noexcept {
    return p >= base && p < (base + static_cast<std::uintptr_t>(len));
}

} // namespace

// Integration test: shm::segment + shm::linear_allocator + Arena::stl_allocator + offset_ptr-backed vector internals.
//
// This simulates a two-process producer/consumer scenario WITHOUT fork/exec by:
// - Mapping the same OS segment twice (producer and consumer views).
// - Destroying the producer view before the consumer dereferences anything.
// If the vector stores raw pointers, they will point into the producer mapping and fail the range checks.
// If the vector stores relocatable pointers (via the allocator's fancy pointer type), it will pass.

int main() {
    struct MyTag {};

    // Readability aliasing, per spec.
    using ShmAllocator = shm::linear_allocator<MyTag, std::uint32_t>;
    using StlAllocatorInt = typename ShmAllocator::template stl_allocator<int>;
    using ShmVector = std::vector<int, StlAllocatorInt>;
    using VecHandle = shm::segment_offset_ptr<ShmVector, MyTag, std::uint32_t>;

    constexpr std::size_t kSegSize = 64ull * 1024ull * 1024ull;

    const std::string seg_name = make_unique_seg_name();

    // Best-effort cleanup from prior crashed runs. On Windows remove() is a no-op by design.
    (void)shm::segment::remove(seg_name.c_str());

    std::unique_ptr<shm::segment> consumer_seg;

    void* producer_base = nullptr;
    void* consumer_base = nullptr;

    {
        shm::segment producer_seg(seg_name.c_str(), kSegSize, shm::segment::open_mode::create_only);

        producer_base = producer_seg.base();
        CHECK(producer_base != nullptr);
        CHECK(producer_seg.size() >= kSegSize);

        // Construct a linear allocator header *inside the shared segment* at its base.
        // The allocator manages the bytes immediately after itself.
        auto* arena = new (producer_base) ShmAllocator(
            producer_base, 
            static_cast<void*>(static_cast<std::byte*>(producer_base) + sizeof(ShmAllocator)),
            producer_seg.size() - sizeof(ShmAllocator)
);


        // The test protocol expects the vector handle to be the first allocation after the arena header.
        // We allocate storage for the handle itself from the arena, and assert its address is exactly base+sizeof(Arena).
        void* handle_loc = arena->alloc(sizeof(VecHandle), alignof(VecHandle));
        CHECK(handle_loc != nullptr);

        const std::uintptr_t expected_handle = uaddr(producer_base) + static_cast<std::uintptr_t>(sizeof(ShmAllocator));
        CHECK(uaddr(handle_loc) == expected_handle);

        auto* vec_handle_slot = new (handle_loc) VecHandle{};

        // Construct the STL allocator wrapper (this must be storable inside the vector).
        StlAllocatorInt stl_alloc(*arena);

        // Construct the vector *inside shared memory*, and persist its handle in shared memory.
        *vec_handle_slot = arena->template make_handle<ShmVector>(stl_alloc);
        CHECK(static_cast<bool>(*vec_handle_slot));

        ShmVector* vec = vec_handle_slot->get();
        CHECK(vec != nullptr);

        // Force a predictable allocation pattern: reserve so we do not explode allocations.
        vec->reserve(4096);

        // Required baseline values.
        vec->push_back(100);
        vec->push_back(200);
        vec->push_back(300);

        // Add a larger tail to stress the allocator + vector internal pointer representation.
        for (int i = 0; i < 2048; ++i) {
            vec->push_back(i ^ 0x55AA);
        }

        CHECK(vec->size() == (3u + 2048u));
        CHECK((*vec)[0] == 100);
        CHECK((*vec)[1] == 200);
        CHECK((*vec)[2] == 300);

        // Create the consumer mapping while the producer is still alive to strongly bias toward a distinct base address.
        consumer_seg.reset(new shm::segment(seg_name.c_str(), kSegSize, shm::segment::open_mode::open_only));
        consumer_base = consumer_seg->base();
        CHECK(consumer_base != nullptr);

        // Two views of the same segment must not overlap.
        CHECK(consumer_base != producer_base);

        // Producer mapping is destroyed here. Any raw pointers stored in the vector will now be dangling.
        // The consumer must still be able to read via relocatable pointers.
    }

    CHECK(consumer_seg != nullptr);

    // Rebind segment base in the "other process" view.
    shm::segment_base<MyTag>::set(consumer_base);
    // Locate the persisted vector handle at the fixed offset.
    void* vec_ptr_location = static_cast<void*>(static_cast<std::byte*>(consumer_base) + sizeof(ShmAllocator));
    auto* consumer_vec_handle =
        std::launder(reinterpret_cast<VecHandle*>(vec_ptr_location));

    CHECK(static_cast<bool>(*consumer_vec_handle));

    ShmVector* consumer_vec = consumer_vec_handle->get();
    CHECK(consumer_vec != nullptr);

    const std::uintptr_t cons_base_u = uaddr(consumer_base);
    const std::size_t cons_size = consumer_seg->size();

    //the vector object itself must live inside the consumer mapping.
    CHECK(in_range(uaddr(consumer_vec), cons_base_u, cons_size));

    // check if the vector's data must also live inside the consumer mapping.
    // If std::vector stored raw pointers, this will point into the (now unmapped) producer view.
    // We perform this range check BEFORE reading elements to avoid crashes on broken implementations.
    int* data = consumer_vec->data();
    CHECK(data != nullptr);
    CHECK(in_range(uaddr(data), cons_base_u, cons_size));

    // Now it is safe to read.
    CHECK(consumer_vec->size() == (3u + 2048u));
    CHECK((*consumer_vec)[0] == 100);
    CHECK((*consumer_vec)[1] == 200);
    CHECK((*consumer_vec)[2] == 300);

    // Spot-check tail correctness.
    CHECK((*consumer_vec)[3] == (0 ^ 0x55AA));
    CHECK((*consumer_vec)[3 + 1024] == (1024 ^ 0x55AA));
    CHECK((*consumer_vec)[3 + 2047] == (2047 ^ 0x55AA));

    // Final cleanup.
    consumer_seg.reset();
    (void)shm::segment::remove(seg_name.c_str());

    std::cout << "[integration] test_stl_vector: PASS (segment=" << seg_name << ")\n";
    return 0;
}
