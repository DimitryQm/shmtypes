#include "shmTypes.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <type_traits>

struct MyTag {};

// Shared-memory safe node: fixed layout, no raw pointers, no vtables.
// This example intentionally keeps the type trivially copyable so that
// whole-region memcpy relocation is a legitimate model for the demo.
struct Payload {
    int   id;
    float data;

    shm::segment_offset_ptr<Payload, MyTag> next;
};

static_assert(std::is_standard_layout_v<Payload>, "Payload must be standard-layout.");
static_assert(std::is_trivially_copyable_v<Payload>, "Payload must be trivially copyable.");

static bool in_region(const void* p, const void* base, std::size_t n) {
    auto x = reinterpret_cast<std::uintptr_t>(p);
    auto b = reinterpret_cast<std::uintptr_t>(base);
    return x >= b && x < (b + n);
}

static void dump_node(const Payload* node, const void* region_base, std::size_t region_size) {
    std::cout << "id=" << node->id
              << " data=" << node->data
              << " this=" << node
              << " next_raw=" << node->next.raw_storage();

    Payload* nxt = node->next.get();
    if (nxt) {
        std::cout << " next=" << nxt
                  << " next_in_region=" << (in_region(nxt, region_base, region_size) ? "yes" : "no");
    } else {
        std::cout << " next=null";
    }
    std::cout << "\n";
}

int main() {
    constexpr std::size_t kRegionSize = 4096;

    alignas(Payload) std::byte region_a[kRegionSize];
    alignas(Payload) std::byte region_b[kRegionSize];

    std::memset(region_a, 0, kRegionSize);
    std::memset(region_b, 0, kRegionSize);

    std::cout << std::hex << std::showbase;
    std::cout << "Region A base: " << static_cast<void*>(region_a) << "\n";
    std::cout << "Region B base: " << static_cast<void*>(region_b) << "\n";
    std::cout << std::dec;

    // Segment-relative anchor requires per-process initialization.
    // In this demo, "Process A" is region_a.
    shm::segment_base<MyTag>::set(region_a);

    // Construct three nodes contiguously using placement new.
    auto* head  = new (region_a) Payload{1, 10.5f, nullptr};
    auto* node2 = new (region_a + sizeof(Payload) * 1) Payload{2, 20.5f, nullptr};
    auto* node3 = new (region_a + sizeof(Payload) * 2) Payload{3, 30.5f, nullptr};

    // Sanity: alignment within the region.
    assert(reinterpret_cast<std::uintptr_t>(head)  % alignof(Payload) == 0);
    assert(reinterpret_cast<std::uintptr_t>(node2) % alignof(Payload) == 0);
    assert(reinterpret_cast<std::uintptr_t>(node3) % alignof(Payload) == 0);

    // Link them via offset_ptr (no absolute addresses stored in the blob).
    head->next  = node2;
    node2->next = node3;
    node3->next = nullptr;

    std::cout << "\nConstructed chain in Region A:\n";
    dump_node(head,  region_a, kRegionSize);
    dump_node(node2, region_a, kRegionSize);
    dump_node(node3, region_a, kRegionSize);

    // "Relocation": copy the entire byte region.
    // In real shared memory, this corresponds to mapping the same underlying bytes
    // at a different base address in another process.
    std::memcpy(region_b, region_a, kRegionSize);

    // "Process B" must set its own base pointer to its own mapping.
    shm::segment_base<MyTag>::set(region_b);

    // Treat the beginning of region_b as the head node.
    // For trivially copyable types, this style of "implicit object from bytes" is a
    // common shared-memory convention. std::launder keeps optimizers honest.
    auto* head_b = std::launder(reinterpret_cast<Payload*>(region_b));

    std::cout << "\nTraversing the migrated chain in Region B:\n";
    Payload* cur = head_b;

    int visited = 0;
    while (cur) {
        assert(in_region(cur, region_b, kRegionSize) && "Traversal escaped Region B (should not happen).");
        dump_node(cur, region_b, kRegionSize);
        cur = cur->next.get();
        ++visited;

        // Hard stop to prevent infinite loops if the structure is corrupted.
        assert(visited < 16);
    }

    assert(visited == 3);

    // Demonstrate that Region B's links do not point back into Region A.
    // If these were raw pointers, you'd typically see next pointers landing in Region A.
    std::cout << "\nVerification: all decoded pointers are within Region B.\n";
    return 0;
}
