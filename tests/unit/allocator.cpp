#include "shmTypes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#define CHECK(expr)                                                                             \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            std::cerr << "CHECK failed: " #expr " @ " << __FILE__ << ":" << __LINE__ << "\n";  \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

struct UnitTag {};

static inline std::uintptr_t uaddr(const void* p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

static inline std::uintptr_t align_up(std::uintptr_t addr, std::size_t alignment) noexcept {
    if (alignment == 0) alignment = 1;
    const std::uintptr_t a = static_cast<std::uintptr_t>(alignment);
    const bool pow2 = (a & (a - 1)) == 0;
    if (pow2) {
        const std::uintptr_t mask = a - 1;
        return (addr + mask) & ~mask;
    }
    const std::uintptr_t rem = addr % a;
    return rem == 0 ? addr : (addr + (a - rem));
}

template <class A>
concept HasSecureReset = requires(A a) { a.secure_reset(); };

static void test_alloc_basic_padding_and_used() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 4096;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    std::size_t cursor = 0;
    const std::uintptr_t base = uaddr(arena);

    auto step = [&](std::size_t size, std::size_t align) {
        const std::size_t before = a.used();
        void* p = a.alloc(size, align);
        const std::size_t after = a.used();

        if (size == 0) {
            CHECK(p == nullptr);
            CHECK(after == before);
            return;
        }

        CHECK(p != nullptr);

        const std::uintptr_t exp_addr = align_up(base + cursor, align);
        const std::size_t exp_start = static_cast<std::size_t>(exp_addr - base);
        CHECK(uaddr(p) == base + exp_start);

        const std::size_t exp_padding = exp_start - cursor;
        const std::size_t exp_next = exp_start + size;

        CHECK(after == exp_next);
        CHECK(after - before == exp_padding + size);

        cursor = exp_next;
    };

    step(1, 1);
    step(7, 8);
    step(13, 16);
    step(64, 32);
    step(5, 0);
    step(9, 24);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_alloc_zero_size_returns_null_and_no_advance() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 1024;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    const std::size_t u0 = a.used();
    void* p0 = a.alloc(0, 1);
    const std::size_t u1 = a.used();

    CHECK(p0 == nullptr);
    CHECK(u1 == u0);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_alloc_oom_does_not_corrupt_cursor() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 256;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    void* p1 = a.alloc(200, 16);
    CHECK(p1 != nullptr);
    const std::size_t u1 = a.used();
    CHECK(u1 <= N);

    void* p2 = a.alloc(N, 1);
    CHECK(p2 == nullptr);
    CHECK(a.used() == u1);

    void* p3 = a.alloc(1, 4096);
    CHECK(p3 == nullptr);
    CHECK(a.used() == u1);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_reset_rewinds_cursor() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 1024;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    void* p1 = a.alloc(32, 32);
    void* p2 = a.alloc(32, 32);
    CHECK(p1 != nullptr);
    CHECK(p2 != nullptr);
    CHECK(a.used() > 0);

    a.reset();
    CHECK(a.used() == 0);

    void* p3 = a.alloc(32, 32);
    CHECK(p3 != nullptr);
    CHECK(p3 == p1);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_secure_reset_scrubs_used_bytes_if_present() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 1024;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0xAB, N);

    Alloc a(arena, N);

    void* p = a.alloc(128, 16);
    CHECK(p != nullptr);
    std::memset(p, 0xCD, 128);

    const std::size_t used_before = a.used();
    CHECK(used_before >= 128);

    if constexpr (HasSecureReset<Alloc>) {
        a.secure_reset();
        CHECK(a.used() == 0);

        for (std::size_t i = 0; i < used_before; ++i) {
            CHECK(arena[i] == std::byte{0});
        }
    }

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_typed_factory_handles() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 4096;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    struct Obj {
        std::uint32_t x;
        std::uint32_t y;
    };

    auto h = a.make_handle<Obj>(Obj{1u, 2u});
    CHECK(h.get() != nullptr);
    CHECK(h->x == 1u);
    CHECK(h->y == 2u);

    auto hv = a.alloc_handle(64, 32);
    CHECK(hv.get() != nullptr);
    CHECK((uaddr(hv.get()) % 32u) == 0);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_stl_allocator_adapter_basic_vector() {
    using Arena = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 1ull * 1024ull * 1024ull;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Arena a(arena, N);

    using A = typename Arena::template stl_allocator<int>;
    std::vector<int, A> v{A(a)};

    for (int i = 0; i < 10'000; ++i) v.push_back(i);

    CHECK(v.size() == 10'000);
    CHECK(v[0] == 0);
    CHECK(v[9999] == 9999);
    CHECK(a.used() > 0);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_allocate_overflow_returns_null() {
    using Alloc = shm::linear_allocator<UnitTag, std::uint32_t>;

    constexpr std::size_t N = 1024;
    std::byte* arena = static_cast<std::byte*>(::operator new(N, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, N);

    Alloc a(arena, N);

    const std::size_t huge = (std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) + 1;
    std::uint64_t* p = a.allocate<std::uint64_t>(huge);
    CHECK(p == nullptr);
    CHECK(a.used() == 0);

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

} // namespace

int main() {
    test_alloc_basic_padding_and_used();
    test_alloc_zero_size_returns_null_and_no_advance();
    test_alloc_oom_does_not_corrupt_cursor();
    test_reset_rewinds_cursor();
    test_secure_reset_scrubs_used_bytes_if_present();
    test_typed_factory_handles();
    test_stl_allocator_adapter_basic_vector();
    test_allocate_overflow_returns_null();
    return 0;
}
