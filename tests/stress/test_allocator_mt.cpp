#include "shmTypes.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
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

struct StressTag {};

struct Rec {
    std::uint32_t start;     // offset from arena base (not segment base)
    std::uint32_t size;      // requested size
    std::uint32_t align;     // requested alignment as passed to alloc()
    std::uint32_t tid;       // thread id (for statistics and debugging)
    std::uint32_t iter;      // allocation index within thread (for debugging)
};

static inline std::uintptr_t uaddr(const void* p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

static inline std::uintptr_t align_up_addr(std::uintptr_t addr, std::size_t alignment) noexcept {
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

static void verify_records(const std::vector<Rec>& recs,
                           std::uintptr_t arena_base_addr,
                           std::size_t arena_capacity,
                           std::size_t final_used)
{
    CHECK(final_used <= arena_capacity);

    std::vector<Rec> s = recs;
    std::sort(s.begin(), s.end(), [](const Rec& a, const Rec& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.size != b.size) return a.size < b.size;
        if (a.align != b.align) return a.align < b.align;
        if (a.tid != b.tid) return a.tid < b.tid;
        return a.iter < b.iter;
    });

    std::size_t cursor = 0;
    std::size_t total_padding = 0;
    std::size_t total_payload = 0;

    for (const Rec& r : s) {
        const std::size_t a = (r.align == 0) ? 1u : static_cast<std::size_t>(r.align);
        const std::size_t sz = static_cast<std::size_t>(r.size);
        const std::size_t start = static_cast<std::size_t>(r.start);

        CHECK(start <= arena_capacity);
        CHECK(sz <= arena_capacity);
        CHECK(start <= arena_capacity - sz);

        const std::uintptr_t cur_addr = arena_base_addr + static_cast<std::uintptr_t>(cursor);
        const std::uintptr_t exp_addr = align_up_addr(cur_addr, a);
        const std::size_t exp_start = static_cast<std::size_t>(exp_addr - arena_base_addr);

        CHECK(start == exp_start);
        CHECK(((arena_base_addr + static_cast<std::uintptr_t>(start)) % static_cast<std::uintptr_t>(a)) == 0);

        const std::size_t padding = start - cursor;
        total_padding += padding;
        total_payload += sz;
        cursor = start + sz;

        CHECK(cursor <= arena_capacity);
    }

    CHECK(cursor == final_used);
    CHECK(cursor == total_padding + total_payload);
}

static std::size_t clamp_threads(std::size_t want) {
    const unsigned hc = std::thread::hardware_concurrency();
    const std::size_t base = (hc == 0 ? 4u : static_cast<std::size_t>(hc));
    const std::size_t cap = 64;
    const std::size_t t = want ? want : (base * 4);
    return std::min<std::size_t>(std::max<std::size_t>(t, 4), cap);
}

static std::uint64_t lcg_step(std::uint64_t& s) noexcept {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s;
}

static void run_mt_alloc_stress(const char* name,
                                std::size_t arena_size_bytes,
                                std::size_t threads,
                                std::size_t iters_per_thread,
                                bool include_non_pow2_align)
{
    using Alloc = shm::linear_allocator<StressTag, std::uint32_t>;
    static_assert(std::is_nothrow_destructible_v<Alloc>);

    std::byte* arena = static_cast<std::byte*>(::operator new(arena_size_bytes, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, arena_size_bytes);

    Alloc alloc(arena, arena_size_bytes);

    const std::uintptr_t base_addr = uaddr(arena);

    std::atomic<bool> go{false};
    std::atomic<std::size_t> ready{0};

    struct ThreadResult {
        std::vector<Rec> recs;
        std::size_t successes = 0;
        std::size_t failures = 0;
    };

    std::vector<ThreadResult> results(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            ThreadResult& out = results[t];
            out.recs.reserve(iters_per_thread);

            std::uint64_t rng = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(t) << 1);

            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t i = 0; i < iters_per_thread; ++i) {
                const std::uint64_t r = lcg_step(rng);

                std::size_t size;
                if ((r & 0xFFu) == 0) {
                    size = 1024 + (static_cast<std::size_t>((r >> 16) & 0x3FFu));
                } else if ((r & 0x3Fu) == 0) {
                    size = 257 + (static_cast<std::size_t>((r >> 8) & 0xFFu));
                } else {
                    size = 1 + (static_cast<std::size_t>(r & 0xFFu));
                }

                if ((i % 997) == 0) size = 0;

                std::size_t align_pow2 = 1ull << (static_cast<std::size_t>((r >> 32) & 0x7u));
                std::size_t align = align_pow2;

                if (include_non_pow2_align) {
                    const std::size_t sel = static_cast<std::size_t>((r >> 40) & 0x3Fu);
                    if (sel == 0) align = 0;
                    else if (sel == 1) align = 3;
                    else if (sel == 2) align = 5;
                    else if (sel == 3) align = 7;
                    else if (sel == 4) align = 24;
                    else if (sel == 5) align = 48;
                    else if (sel == 6) align = 96;
                }

                if ((i % 4096) == 123) align = 4096;

                const std::size_t used_before = alloc.used();
                void* p = alloc.alloc(size, align);
                const std::size_t used_after = alloc.used();

                if (!p) {
                    out.failures++;
                    CHECK(used_after >= used_before);
                    continue;
                }

                out.successes++;
                CHECK(size != 0);

                const std::uintptr_t pa = uaddr(p);
                CHECK(pa >= base_addr);
                CHECK(pa < base_addr + arena_size_bytes);

                const std::uintptr_t a = static_cast<std::uintptr_t>((align == 0) ? 1 : align);
                CHECK((pa % a) == 0);

                const std::uintptr_t start_u = pa - base_addr;
                CHECK(start_u + size <= static_cast<std::uintptr_t>(arena_size_bytes));

                std::memset(p, static_cast<int>((t * 1315423911u) ^ static_cast<unsigned>(i)), size);

                out.recs.push_back(Rec{
                    static_cast<std::uint32_t>(start_u),
                    static_cast<std::uint32_t>(size),
                    static_cast<std::uint32_t>(align),
                    static_cast<std::uint32_t>(t),
                    static_cast<std::uint32_t>(i),
                });
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& th : pool) th.join();

    std::vector<Rec> all;
    std::size_t total_success = 0;
    std::size_t total_fail = 0;

    for (auto& r : results) {
        total_success += r.successes;
        total_fail += r.failures;
        all.insert(all.end(),
                   std::make_move_iterator(r.recs.begin()),
                   std::make_move_iterator(r.recs.end()));
    }

    const std::size_t final_used = alloc.used();
    verify_records(all, base_addr, arena_size_bytes, final_used);
    CHECK(total_success > 0);

    std::cout << "[stress] " << name
              << " threads=" << threads
              << " iters=" << iters_per_thread
              << " success=" << total_success
              << " fail=" << total_fail
              << " used=" << final_used
              << " / " << arena_size_bytes
              << "\n";

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

static void test_mt_random_pow2_align() {
    const std::size_t arena = 64ull * 1024ull * 1024ull;
    const std::size_t threads = clamp_threads(0);
    const std::size_t iters = 10'000;
    run_mt_alloc_stress("random_pow2_align", arena, threads, iters, false);
}

static void test_mt_random_mixed_align() {
    const std::size_t arena = 64ull * 1024ull * 1024ull;
    const std::size_t threads = clamp_threads(0);
    const std::size_t iters = 10'000;
    run_mt_alloc_stress("random_mixed_align", arena, threads, iters, true);
}

static void test_mt_hot_contention_fixed_size() {
    using Alloc = shm::linear_allocator<StressTag, std::uint32_t>;
    constexpr std::size_t arena_size = 64ull * 1024ull * 1024ull;
    std::byte* arena = static_cast<std::byte*>(::operator new(arena_size, std::align_val_t(alignof(std::max_align_t))));
    std::memset(arena, 0, arena_size);

    Alloc alloc(arena, arena_size);

    const std::uintptr_t base_addr = uaddr(arena);

    const std::size_t threads = clamp_threads(0);
    const std::size_t iters = 200'000 / threads;
    const std::size_t sz = 64;
    const std::size_t al = 64;

    std::atomic<bool> go{false};
    std::atomic<std::size_t> ready{0};

    std::vector<std::vector<Rec>> per_thread(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            auto& recs = per_thread[t];
            recs.reserve(iters);

            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t i = 0; i < iters; ++i) {
                void* p = alloc.alloc(sz, al);
                if (!p) break;

                std::memset(p, static_cast<int>(t), sz);

                const std::uintptr_t start_u = uaddr(p) - base_addr;
                recs.push_back(Rec{
                    static_cast<std::uint32_t>(start_u),
                    static_cast<std::uint32_t>(sz),
                    static_cast<std::uint32_t>(al),
                    static_cast<std::uint32_t>(t),
                    static_cast<std::uint32_t>(i),
                });
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& th : pool) th.join();

    std::vector<Rec> all;
    all.reserve(threads * iters);
    for (auto& v : per_thread) {
        all.insert(all.end(),
                   std::make_move_iterator(v.begin()),
                   std::make_move_iterator(v.end()));
    }

    const std::size_t final_used = alloc.used();
    verify_records(all, base_addr, arena_size, final_used);

    std::cout << "[stress] hot_contention_fixed_size"
              << " threads=" << threads
              << " iters_per_thread=" << iters
              << " allocations=" << all.size()
              << " used=" << final_used
              << " / " << arena_size
              << "\n";

    ::operator delete(arena, std::align_val_t(alignof(std::max_align_t)));
}

} // namespace

int main() {
    test_mt_random_pow2_align();
    test_mt_random_mixed_align();
    test_mt_hot_contention_fixed_size();
    return 0;
}
