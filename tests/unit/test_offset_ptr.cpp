#include "shmTypes.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <type_traits>

namespace {

#define CHECK(expr)                                                                             \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            std::cerr << "CHECK failed: " #expr " @ " << __FILE__ << ":" << __LINE__ << "\n";  \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

struct TagA {};
struct TagB {};

template <class U>
concept OffsetPtrWellFormed = requires { typename shm::offset_ptr<U>; };

template <class P>
concept HasArrow = requires(P p) { p.operator->(); };

template <class P>
concept HasDeref = requires(P p) { *p; };

template <class P>
concept HasGet = requires(P p) { p.get(); };

static bool in_region(const void* p, const void* base, std::size_t n) {
    auto x = reinterpret_cast<std::uintptr_t>(p);
    auto b = reinterpret_cast<std::uintptr_t>(base);
    return x >= b && x < (b + n);
}

static void test_null_semantics() {
    shm::offset_ptr<int> p0;
    CHECK(!p0);
    CHECK(p0.get() == nullptr);
    CHECK(p0.raw_storage() == 0);

    shm::offset_ptr<int> p1(nullptr);
    CHECK(!p1);
    CHECK(p1.get() == nullptr);
    CHECK(p1.raw_storage() == 0);

    int x = 7;
    shm::offset_ptr<int> p2(&x);
    CHECK(!!p2);
    CHECK(p2.get() == &x);
    p2 = nullptr;
    CHECK(!p2);
    CHECK(p2.get() == nullptr);
}

static void test_self_anchor_copy_rebases() {
    int x = 123;

    using P = shm::offset_ptr<int, shm::self_anchor, std::int32_t>;
    P a(&x);
    CHECK(a.get() == &x);

    P b(a);
    CHECK(b.get() == &x);

    P c;
    c = a;
    CHECK(c.get() == &x);

    P d(std::move(a));
    CHECK(d.get() == &x);
}

static void test_segment_anchor_triviality_contract() {
    using P = shm::segment_offset_ptr<int, TagA, std::uint32_t>;
    static_assert(std::is_trivially_copyable_v<P>,
                  "segment_anchor mode should be trivially copyable for container relocation.");
    static_assert(std::is_trivially_move_constructible_v<P>);
    static_assert(std::is_trivially_copy_assignable_v<P>);
    static_assert(std::is_trivially_move_assignable_v<P>);
}

static void test_relocation_memcpy_segment_anchor() {
    struct Node {
        int v;
        shm::segment_offset_ptr<Node, TagA, std::uint32_t> next;
    };

    static_assert(std::is_standard_layout_v<Node>);
    static_assert(std::is_trivially_copyable_v<Node>);

    constexpr std::size_t N = 4096;
    alignas(Node) std::byte region_a[N];
    alignas(Node) std::byte region_b[N];
    std::memset(region_a, 0, N);
    std::memset(region_b, 0, N);

    shm::segment_base<TagA>::set(region_a);

    auto* n1 = new (region_a + 0 * sizeof(Node)) Node{1, nullptr};
    auto* n2 = new (region_a + 1 * sizeof(Node)) Node{2, nullptr};
    auto* n3 = new (region_a + 2 * sizeof(Node)) Node{3, nullptr};

    n1->next = n2;
    n2->next = n3;
    n3->next = nullptr;

    CHECK(in_region(n1->next.get(), region_a, N));
    CHECK(in_region(n2->next.get(), region_a, N));
    CHECK(n3->next.get() == nullptr);

    std::memcpy(region_b, region_a, N);

    shm::segment_base<TagA>::set(region_b);

    auto* head_b = std::launder(reinterpret_cast<Node*>(region_b));
    int sum = 0;
    int steps = 0;

    for (Node* cur = head_b; cur; cur = cur->next.get()) {
        CHECK(in_region(cur, region_b, N));
        sum += cur->v;
        ++steps;
        CHECK(steps < 32);
    }

    CHECK(steps == 3);
    CHECK(sum == 6);
}

static void test_equality_identity_vs_offset_identity_self_anchor() {
    constexpr std::size_t N = 256;
    alignas(std::max_align_t) std::byte buf[N];
    std::memset(buf, 0, N);

    using P = shm::offset_ptr<int, shm::self_anchor, std::int32_t>;

    auto* pA = new (buf + 16) P(nullptr);
    auto* pB = new (buf + 64) P(nullptr);
    auto* target = new (buf + 128) int(0x11223344);

    *pA = target;
    *pB = target;

    CHECK(pA->get() == target);
    CHECK(pB->get() == target);
    CHECK((*pA) == (*pB));
    CHECK(pA->raw_storage() != pB->raw_storage());
}

static void test_inheritance_upcast_derived_to_base() {
    struct Base {
        std::uint32_t a;
    };
    struct Derived : Base {
        std::uint32_t b;
    };

    static_assert(std::is_standard_layout_v<Base>);
    static_assert(std::is_trivially_copyable_v<Base>);
    static_assert(std::is_trivially_copyable_v<Derived>);

    constexpr std::size_t N = 256;
    alignas(Derived) std::byte region[N];
    std::memset(region, 0, N);
    shm::segment_base<TagB>::set(region);

    auto* d = new (region) Derived{{1u}, 2u};

    shm::segment_offset_ptr<Derived, TagB, std::uint32_t> pd(d);
    shm::segment_offset_ptr<Base, TagB, std::uint32_t> pb = pd;

    CHECK(pd.get() == d);
    CHECK(pb.get() == static_cast<Base*>(d));
    CHECK(pb->a == 1u);
}

static void test_const_correctness() {
    static_assert(std::is_convertible_v<shm::offset_ptr<int>, shm::offset_ptr<const int>>,
                  "offset_ptr<T> must convert to offset_ptr<const T>.");
    static_assert(!std::is_convertible_v<shm::offset_ptr<const int>, shm::offset_ptr<int>>,
                  "offset_ptr<const T> must not convert to offset_ptr<T>.");

    int x = 9;
    shm::offset_ptr<int> pm(&x);
    shm::offset_ptr<const int> pc = pm;

    CHECK(pm.get() == &x);
    CHECK(pc.get() == &x);
    CHECK(*pc == 9);
}

static void test_void_pointer_semantics() {
    static_assert(OffsetPtrWellFormed<void>, "offset_ptr<void> must be a valid type.");
    using PV = shm::offset_ptr<void, shm::self_anchor, std::int32_t>;
    static_assert(HasGet<PV>, "offset_ptr<void> must provide get().");
    static_assert(!HasArrow<PV>, "offset_ptr<void> must not provide operator->.");
    static_assert(!HasDeref<PV>, "offset_ptr<void> must not provide operator*.");

    int x = 42;
    PV pv(static_cast<void*>(&x));
    CHECK(pv.get() == static_cast<void*>(&x));

    shm::offset_ptr<int, shm::self_anchor, std::int32_t> pi(static_cast<int*>(pv.get()));
    CHECK(pi.get() == &x);
    CHECK(*pi == 42);
}

static void test_recursive_relocation_inception() {
    constexpr std::size_t N = 512;
    alignas(std::max_align_t) std::byte region_a[N];
    alignas(std::max_align_t) std::byte region_b[N];
    std::memset(region_a, 0, N);
    std::memset(region_b, 0, N);

    shm::segment_base<TagA>::set(region_a);

    using PI = shm::segment_offset_ptr<int, TagA, std::uint32_t>;
    using PPI = shm::segment_offset_ptr<PI, TagA, std::uint32_t>;

    auto* intC = new (region_a + 128) int(777);
    auto* ptrB = new (region_a + 64) PI(intC);
    auto* ptrA = new (region_a + 16) PPI(ptrB);

    CHECK(ptrA->get() == ptrB);
    CHECK(ptrB->get() == intC);
    CHECK(**ptrA->get() == 777);

    std::memcpy(region_b, region_a, N);
    shm::segment_base<TagA>::set(region_b);

    auto* ptrA_b = std::launder(reinterpret_cast<PPI*>(region_b + 16));
    auto* ptrB_b = ptrA_b->get();
    CHECK(in_region(ptrB_b, region_b, N));

    auto* intC_b = ptrB_b->get();
    CHECK(in_region(intC_b, region_b, N));
    CHECK(*intC_b == 777);

    CHECK(**ptrA_b->get() == 777);
}

static void test_segment_base_rebinding_is_per_tag() {
    constexpr std::size_t N = 256;
    alignas(std::max_align_t) std::byte region_a[N];
    alignas(std::max_align_t) std::byte region_b[N];
    std::memset(region_a, 0, N);
    std::memset(region_b, 0, N);

    shm::segment_base<TagA>::set(region_a);
    shm::segment_base<TagB>::set(region_b);

    int* a = new (region_a + 64) int(1);
    int* b = new (region_b + 64) int(2);

    shm::segment_offset_ptr<int, TagA, std::uint32_t> pa(a);
    shm::segment_offset_ptr<int, TagB, std::uint32_t> pb(b);

    CHECK(pa.get() == a);
    CHECK(pb.get() == b);
    CHECK(*pa.get() == 1);
    CHECK(*pb.get() == 2);
}

static void test_self_anchor_two_hop_chain() {
    struct Box {
        shm::self_reloc_ptr<Box, std::int32_t> next;
        int payload;
    };
    
    static_assert(std::is_trivially_copyable_v<Box>);
    

    static_assert(std::is_standard_layout_v<Box>);
    static_assert(std::is_trivially_copyable_v<Box>);

    constexpr std::size_t N = 256;
    alignas(Box) std::byte a[N];
    alignas(Box) std::byte b[N];
    std::memset(a, 0, N);
    std::memset(b, 0, N);

    auto* b1 = new (a + 0 * sizeof(Box)) Box{nullptr, 11};
    auto* b2 = new (a + 1 * sizeof(Box)) Box{nullptr, 22};
    b1->next = b2;
    b2->next = nullptr;

    std::memcpy(b, a, N);

    auto* head = std::launder(reinterpret_cast<Box*>(b));
    CHECK(head->payload == 11);
    CHECK(head->next.get() != nullptr);
    CHECK(head->next.get()->payload == 22);
    CHECK(head->next.get()->next.get() == nullptr);
}

} // namespace

int main() {
    test_null_semantics();
    test_self_anchor_copy_rebases();
    test_segment_anchor_triviality_contract();
    test_relocation_memcpy_segment_anchor();
    test_equality_identity_vs_offset_identity_self_anchor();
    test_inheritance_upcast_derived_to_base();
    test_const_correctness();
    test_void_pointer_semantics();
    test_recursive_relocation_inception();
    test_segment_base_rebinding_is_per_tag();
    test_self_anchor_two_hop_chain();
    return 0;
}
