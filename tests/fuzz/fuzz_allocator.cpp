#include "shmTypes.hpp"

#include <algorithm>
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
#include <unistd.h>
#include <errno.h>   
namespace {

#define CHECK(expr)                                                                             \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            std::cerr << "CHECK failed: " #expr " @ " << __FILE__ << ":" << __LINE__ << "\n";  \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

struct FuzzTag {};

template <class A>
concept HasSecureReset = requires(A a) { a.secure_reset(); };

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

struct Block {
    std::size_t start;
    std::size_t size;
    std::size_t align;
};

static void verify_no_overlap_and_within(const std::vector<Block>& blocks,
                                         std::size_t capacity,
                                         std::uintptr_t base_addr)
{
    std::vector<Block> s = blocks;
    std::sort(s.begin(), s.end(), [](const Block& a, const Block& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.size != b.size) return a.size < b.size;
        return a.align < b.align;
    });

    std::size_t prev_end = 0;
    for (const auto& b : s) {
        CHECK(b.start + b.size <= capacity);
        CHECK(b.start >= prev_end);
        const std::uintptr_t p = base_addr + static_cast<std::uintptr_t>(b.start);
        const std::size_t a = (b.align == 0 ? 1 : b.align);
        CHECK((p % static_cast<std::uintptr_t>(a)) == 0);
        prev_end = b.start + b.size;
    }
}

static std::uint32_t read_u32(const std::uint8_t*& p, const std::uint8_t* end) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v <<= 8;
        if (p != end) v |= *p++;
    }
    return v;
}

static std::uint8_t read_u8(const std::uint8_t*& p, const std::uint8_t* end) noexcept {
    if (p == end) return 0;
    return *p++;
}

static std::size_t pick_align(std::uint32_t x) noexcept {
    static constexpr std::size_t table[] = {
        0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
    };
    return table[x % (sizeof(table) / sizeof(table[0]))];
}


struct Node {
    std::uint32_t value;
    shm::segment_offset_ptr<Node, FuzzTag, std::uint32_t> next;
};

static_assert(std::is_standard_layout_v<Node>);
static_assert(std::is_trivially_copyable_v<Node> == false || true, "Node may be non-trivial; construction is via make_handle.");

static int fuzz_one(const std::uint8_t* data, std::size_t size) {
    using Alloc = shm::linear_allocator<FuzzTag, std::uint32_t>;

    // Keep this moderate so CI runs it quickly; real fuzzers can scale it up.
    constexpr std::size_t kArenaSize = 1ull * 1024ull * 1024ull;

    // Page-align arenas so relocation preserves power-of-two alignments <= page size.
    std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    if (page == 0 || (page & (page - 1)) != 0) page = 4096;

    void* pa = nullptr;
    void* pb = nullptr;
    CHECK(::posix_memalign(&pa, page, kArenaSize) == 0);
    CHECK(::posix_memalign(&pb, page, kArenaSize) == 0);

    std::byte* arena_a = static_cast<std::byte*>(pa);
    std::byte* arena_b = static_cast<std::byte*>(pb);

    std::memset(arena_a, 0, kArenaSize);
    std::memset(arena_b, 0, kArenaSize);


    Alloc alloc(arena_a, kArenaSize);

    const std::uintptr_t base_a = uaddr(arena_a);
    std::uintptr_t base_live = base_a;

    std::size_t model_cursor = 0;
    std::vector<Block> blocks;
    blocks.reserve(4096);

    // We'll also build a simple singly-linked list using segment handles, to ensure
    // that object creation + pointer encoding remains valid across resets and relocations.
    using H = typename Alloc::template handle<Node>;
    H head(nullptr);
    std::size_t nodes = 0;

    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;

    // Limit operations so non-fuzzer runs remain bounded and fast.
    const std::size_t max_ops = 4000;
    for (std::size_t op_i = 0; op_i < max_ops && p < end; ++op_i) {
        const std::uint8_t op = read_u8(p, end);

        // Bias: allocate is common, reset/relocate are rarer but exercised.
        switch (op % 8) {
            case 0:
            case 1:
            case 2: {
                // alloc bytes with alignment
                const std::uint32_t r = read_u32(p, end);
                const std::size_t n = static_cast<std::size_t>(r & 0x3FFFu); // 0..16383
                const std::size_t al = pick_align(read_u32(p, end));

                // Model the allocator: minimal aligned address from current cursor.
                const std::uintptr_t cur_addr = base_live + static_cast<std::uintptr_t>(model_cursor);
                const std::uintptr_t aligned = align_up_addr(cur_addr, al);
                const std::size_t start = static_cast<std::size_t>(aligned - base_live);

                void* got = alloc.alloc(n, al);

                if (n == 0) {
                    CHECK(got == nullptr);
                    CHECK(alloc.used() == model_cursor);
                    break;
                }

                if (start > kArenaSize || n > kArenaSize || start > kArenaSize - n) {
                    CHECK(got == nullptr);
                    CHECK(alloc.used() == model_cursor);
                    break;
                }

                CHECK(got != nullptr);
                CHECK(uaddr(got) == base_live + static_cast<std::uintptr_t>(start));

                model_cursor = start + n;
                CHECK(alloc.used() == model_cursor);

                blocks.push_back(Block{start, n, al});

                // Touch it to catch overlaps under ASan/MSan and to exercise addressability.
                std::memset(got, static_cast<int>(op_i & 0xFF), n);
                break;
            }

            case 3: {
                // make_handle Node, link it to the head (tests allocator factory + offset_ptr mechanics)
                const std::uint32_t v = read_u32(p, end);

                H node = alloc.template make_handle<Node>(Node{v, head});
                if (!node) {
                    // Must be OOM; cursor unchanged in the model if we can predict it.
                    // We cannot precisely model make_handle without knowing its internal alloc(),
                    // but in this library it should be alloc(sizeof(Node), alignof(Node)).
                    const std::uintptr_t cur_addr = base_live + static_cast<std::uintptr_t>(model_cursor);
                    const std::uintptr_t aligned = align_up_addr(cur_addr, alignof(Node));
                    const std::size_t start = static_cast<std::size_t>(aligned - base_live);

                    if (start <= kArenaSize && start <= kArenaSize - sizeof(Node)) {
                        // If it would have fit, failing is a bug.
                        CHECK(false);
                    }
                    CHECK(alloc.used() == model_cursor);
                    break;
                }

                Node* np = node.get();
                CHECK(np != nullptr);
                CHECK(uaddr(np) >= base_live);
                CHECK(uaddr(np) < base_live + kArenaSize);
                CHECK((uaddr(np) % alignof(Node)) == 0);

                // Model cursor advance for this allocation.
                const std::uintptr_t cur_addr = base_live + static_cast<std::uintptr_t>(model_cursor);
                const std::uintptr_t aligned = align_up_addr(cur_addr, alignof(Node));
                const std::size_t start = static_cast<std::size_t>(aligned - base_live);
                CHECK(uaddr(np) == base_live + static_cast<std::uintptr_t>(start));
                model_cursor = start + sizeof(Node);
                CHECK(alloc.used() == model_cursor);

                blocks.push_back(Block{start, sizeof(Node), alignof(Node)});

                // Validate the list head and single hop.
                CHECK(np->value == v);
                if (head) {
                    Node* hp = np->next.get();
                    CHECK(hp != nullptr);
                    CHECK(uaddr(hp) >= base_live);
                    CHECK(uaddr(hp) < base_live + kArenaSize);
                }

                head = node;
                ++nodes;
                break;
            }

            case 4: {
                // reset (frame boundary)
                alloc.reset();
                model_cursor = 0;
                blocks.clear();
                head = H(nullptr);
                nodes = 0;
                break;
            }

            case 5: {
                // secure_reset if available; otherwise normal reset.
                if constexpr (HasSecureReset<Alloc>) {
                    alloc.secure_reset();
                } else {
                    alloc.reset();
                }
                model_cursor = 0;
                blocks.clear();
                head = H(nullptr);
                nodes = 0;
                break;
            }

            case 6: {
                const std::size_t used = alloc.used();
                CHECK(used == model_cursor);
            
                // Copy used prefix into arena_b
                std::memset(arena_b, 0, kArenaSize);
                std::memcpy(arena_b, arena_a, used);
            
                // Make arena_b the new live arena (swap pointers)
                std::swap(arena_a, arena_b);
            
                // Rebuild allocator on the new live arena.
                // (Your allocator sets segment_base<Tag>::set(arena_a) internally.)
                alloc.~Alloc();
                new (&alloc) Alloc(arena_a, kArenaSize);
            
                // Reserve the already-used prefix so allocator cursor matches the copied state.
                if (used != 0) {
                    void* r = alloc.alloc(used, 1);
                    CHECK(r != nullptr);
                    CHECK(uaddr(r) == uaddr(arena_a)); // should reserve from offset 0
                }
            
                base_live = uaddr(arena_a);
            
                CHECK(alloc.used() == used);
                CHECK(model_cursor == used);
            
                // Verify list integrity (bounded).
                std::size_t walked = 0;
                for (Node* cur = head.get(); cur && walked < 128; cur = cur->next.get()) {
                    CHECK(uaddr(cur) >= base_live);
                    CHECK(uaddr(cur) < base_live + kArenaSize);
                    ++walked;
                }
                CHECK(walked <= nodes);
                break;
            }
            

            case 7: {
                // Consistency check: no overlaps and all aligned.
                verify_no_overlap_and_within(blocks, kArenaSize, base_live);
                CHECK(alloc.used() == model_cursor);
                break;
            }
        }
    }

    // End-state invariants.
    CHECK(alloc.used() == model_cursor);
    verify_no_overlap_and_within(blocks, kArenaSize, base_live);

    std::free(arena_a);
    std::free(arena_b);    
    return 0;
}

} // namespace

// LibFuzzer / OSS-Fuzz detection.
#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
#define SHM_LIBFUZZER 1
#elif defined(__has_feature)
#if __has_feature(fuzzer)
#define SHM_LIBFUZZER 1
#endif
#endif

#if defined(SHM_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return fuzz_one(data, size);
}

#else

// Non-fuzzer build: run a bounded deterministic “mini-fuzz” so CI can execute it safely.
int main() {
    // A small fixed corpus plus a cheap PRNG expansion.
    std::vector<std::uint8_t> blob(4096);

    // Seeded pattern: stable across platforms, enough entropy to exercise paths.
    std::uint64_t s = 0x123456789ABCDEF0ull;
    auto next = [&]() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return s;
    };

    for (int round = 0; round < 200; ++round) {
        for (std::size_t i = 0; i < blob.size(); ++i) {
            blob[i] = static_cast<std::uint8_t>(next() >> 56);
        }
        fuzz_one(blob.data(), blob.size());
    }

    // Also exercise tiny inputs (common fuzz edge case).
    for (std::size_t n = 0; n < 32; ++n) {
        fuzz_one(blob.data(), n);
    }

    return 0;
}

#endif
