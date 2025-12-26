#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <atomic>
#include <new>
#include <utility>
#include <cstring>

#if defined(_MSC_VER)
  #define SHM_FORCE_INLINE __forceinline
  #define SHM_NOINLINE __declspec(noinline)
  #define SHM_ASSUME(x) __assume(x)
  #define SHM_LIKELY(x)   (x)
  #define SHM_UNLIKELY(x) (x)
#elif defined(__GNUC__) || defined(__clang__)
  #define SHM_FORCE_INLINE inline __attribute__((always_inline))
  #define SHM_NOINLINE __attribute__((noinline))
  #if defined(__clang__)
    #define SHM_ASSUME(x) __builtin_assume(x)
  #else
    #define SHM_ASSUME(x) do { if(!(x)) __builtin_unreachable(); } while(0)
  #endif
  #define SHM_LIKELY(x)   (__builtin_expect(!!(x), 1))
  #define SHM_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
  #define SHM_FORCE_INLINE inline
  #define SHM_NOINLINE
  #define SHM_ASSUME(x) ((void)0)
  #define SHM_LIKELY(x)   (x)
  #define SHM_UNLIKELY(x) (x)
#endif

#ifndef SHM_OFFSET_PTR_DEBUG
  #define SHM_OFFSET_PTR_DEBUG 0
#endif

#if SHM_OFFSET_PTR_DEBUG
  #include <cassert>
  #define SHM_ASSERT(x) assert(x)
#else
  #define SHM_ASSERT(x) ((void)0)
#endif

namespace shm {

namespace detail {
    using uptr = std::uintptr_t;
    using iptr = std::intptr_t;

    template <class I>
    concept offset_int =
        std::is_integral_v<I> &&
        !std::is_same_v<I, bool> &&
        (sizeof(I) <= sizeof(iptr));

    SHM_FORCE_INLINE constexpr uptr addr(const void* p) noexcept {
        static_assert(sizeof(uptr) == sizeof(p));
        return std::bit_cast<uptr>(p);
    }

    template <class To>
    SHM_FORCE_INLINE constexpr To narrow_checked(iptr v) noexcept {
#if SHM_OFFSET_PTR_DEBUG
        if constexpr (std::is_signed_v<To>) {
            SHM_ASSERT(v >= static_cast<iptr>(std::numeric_limits<To>::min()));
            SHM_ASSERT(v <= static_cast<iptr>(std::numeric_limits<To>::max()));
        } else {
            SHM_ASSERT(v >= 0);
            SHM_ASSERT(static_cast<std::make_unsigned_t<iptr>>(v)
                       <= std::numeric_limits<To>::max());
        }
#endif
        return static_cast<To>(v);
    }

    template <class T>
    inline constexpr bool is_obj_or_void_v =
        (std::is_object_v<T> || std::is_void_v<T>);
} // namespace detail

template <class Tag>
struct segment_base {
    static SHM_FORCE_INLINE void set(void* base) noexcept {
        base_ = static_cast<std::byte*>(base);
    }
    static SHM_FORCE_INLINE std::byte* get() noexcept { return base_; }

private:
    inline static std::byte* base_ = nullptr;
};

struct self_anchor {
    static constexpr bool kSelfRelative = true;
    static SHM_FORCE_INLINE detail::uptr base(const void* self) noexcept {
        return detail::addr(self);
    }
};

struct self_reloc_anchor {
    static constexpr bool kSelfRelative = true;
    static SHM_FORCE_INLINE detail::uptr base(const void* self) noexcept {
        return detail::addr(self);
    }
};


template <class Tag>
struct segment_anchor {
    static constexpr bool kSelfRelative = false;
    static SHM_FORCE_INLINE detail::uptr base(const void*) noexcept {
        auto* b = segment_base<Tag>::get();
        SHM_ASSERT(b && "segment_base<Tag>::set(mapped_base) must be called before use.");
        return detail::addr(b);
    }
};

template <class T, class Anchor = self_anchor, detail::offset_int OffsetT = std::int32_t>
class offset_ptr {
public:
    using element_type = T;
    using pointer      = T*;
    using reference = std::add_lvalue_reference_t<T>;
    using offset_type  = OffsetT;

    static_assert(detail::is_obj_or_void_v<T>,
                  "offset_ptr<T>: T must be an object type or void.");

    constexpr offset_ptr() noexcept = default;
    constexpr offset_ptr(std::nullptr_t) noexcept : off_plus1_(0) {}

    SHM_FORCE_INLINE explicit offset_ptr(pointer p) noexcept { set(p); }

    SHM_FORCE_INLINE offset_ptr(const offset_ptr& other) noexcept { set(other.get()); }
    SHM_FORCE_INLINE offset_ptr& operator=(const offset_ptr& other) noexcept {
        if (this != &other) set(other.get());
        return *this;
    }

    SHM_FORCE_INLINE offset_ptr& operator=(pointer p) noexcept { set(p); return *this; }
    SHM_FORCE_INLINE offset_ptr& operator=(std::nullptr_t) noexcept { off_plus1_ = 0; return *this; }

    template <class U>
    requires (std::is_convertible_v<U*, T*>)
    SHM_FORCE_INLINE offset_ptr(const offset_ptr<U, Anchor, OffsetT>& other) noexcept {
        set(other.get());
    }

    [[nodiscard]] SHM_FORCE_INLINE pointer get() const noexcept {
        const offset_type s = off_plus1_;
        if (SHM_UNLIKELY(s == 0)) return nullptr;

        const detail::uptr b = Anchor::base(this);

        if constexpr (std::is_signed_v<offset_type>) {
            const detail::iptr off = static_cast<detail::iptr>(s) - 1;
            return reinterpret_cast<pointer>(b + static_cast<detail::uptr>(off));
        } else {
            const detail::uptr off = static_cast<detail::uptr>(s - 1);
            return reinterpret_cast<pointer>(b + off);
        }
    }

    [[nodiscard]] SHM_FORCE_INLINE offset_type raw_storage() const noexcept { return off_plus1_; }

    [[nodiscard]] SHM_FORCE_INLINE explicit operator bool() const noexcept { return off_plus1_ != 0; }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U& operator*() const noexcept { return *get(); }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U* operator->() const noexcept { return get(); }

private:
    SHM_FORCE_INLINE void set(pointer p) noexcept {
        if (!p) { off_plus1_ = 0; return; }

        const detail::uptr b = Anchor::base(this);
        const detail::uptr t = detail::addr(p);
        const detail::iptr diff = static_cast<detail::iptr>(t) - static_cast<detail::iptr>(b);

        // encode: stored = diff + 1 (0 reserved for null)
        if constexpr (std::is_signed_v<offset_type>) {
            SHM_ASSERT(diff != -1 && "diff == -1 would encode to 0 (reserved for null).");
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        } else {
            SHM_ASSERT(diff >= 0);
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        }
    }

    offset_type off_plus1_ = 0;
};

template <class T, class Tag, detail::offset_int OffsetT>
class offset_ptr<T, segment_anchor<Tag>, OffsetT> {
public:
    using element_type = T;
    using pointer      = T*;
    using reference = std::add_lvalue_reference_t<T>;
    using offset_type  = OffsetT;

    static_assert(detail::is_obj_or_void_v<T>,
                  "offset_ptr<T>: T must be an object type or void.");

    constexpr offset_ptr() noexcept = default;
    constexpr offset_ptr(std::nullptr_t) noexcept : off_plus1_(0) {}
    SHM_FORCE_INLINE explicit offset_ptr(pointer p) noexcept { set(p); }

    offset_ptr(const offset_ptr&) noexcept = default;
    offset_ptr(offset_ptr&&) noexcept = default;
    offset_ptr& operator=(const offset_ptr&) noexcept = default;
    offset_ptr& operator=(offset_ptr&&) noexcept = default;
    ~offset_ptr() = default;

    template <class U>
    requires (std::is_convertible_v<U*, T*>)
    SHM_FORCE_INLINE offset_ptr(const offset_ptr<U, segment_anchor<Tag>, OffsetT>& other) noexcept
        : off_plus1_(other.raw_storage()) {}

    SHM_FORCE_INLINE offset_ptr& operator=(pointer p) noexcept { set(p); return *this; }
    SHM_FORCE_INLINE offset_ptr& operator=(std::nullptr_t) noexcept { off_plus1_ = 0; return *this; }

    [[nodiscard]] SHM_FORCE_INLINE pointer get() const noexcept {
        const offset_type s = off_plus1_;
        if (SHM_UNLIKELY(s == 0)) return nullptr;

        const detail::uptr b = segment_anchor<Tag>::base(nullptr);

        if constexpr (std::is_signed_v<offset_type>) {
            const detail::iptr off = static_cast<detail::iptr>(s) - 1;
            return reinterpret_cast<pointer>(b + static_cast<detail::uptr>(off));
        } else {
            const detail::uptr off = static_cast<detail::uptr>(s - 1);
            return reinterpret_cast<pointer>(b + off);
        }
    }

    [[nodiscard]] SHM_FORCE_INLINE offset_type raw_storage() const noexcept { return off_plus1_; }
    [[nodiscard]] SHM_FORCE_INLINE explicit operator bool() const noexcept { return off_plus1_ != 0; }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U& operator*() const noexcept { return *get(); }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U* operator->() const noexcept { return get(); }

private:
    SHM_FORCE_INLINE void set(pointer p) noexcept {
        if (!p) { off_plus1_ = 0; return; }

        const detail::uptr b = segment_anchor<Tag>::base(nullptr);
        const detail::uptr t = detail::addr(p);
        const detail::iptr diff = static_cast<detail::iptr>(t) - static_cast<detail::iptr>(b);

        if constexpr (std::is_signed_v<offset_type>) {
            SHM_ASSERT(diff != -1 && "diff == -1 would encode to 0 (reserved for null).");
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        } else {
            SHM_ASSERT(diff >= 0);
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        }
    }

    offset_type off_plus1_ = 0;
};

template <class T, detail::offset_int OffsetT>
class offset_ptr<T, self_reloc_anchor, OffsetT> {
public:
    using element_type = T;
    using pointer      = T*;
    using reference = std::add_lvalue_reference_t<T>;
    using offset_type  = OffsetT;

    static_assert(detail::is_obj_or_void_v<T>,
                  "offset_ptr<T>: T must be an object type or void.");

    constexpr offset_ptr() noexcept = default;
    constexpr offset_ptr(std::nullptr_t) noexcept : off_plus1_(0) {}
    SHM_FORCE_INLINE explicit offset_ptr(pointer p) noexcept { set(p); }

    offset_ptr(const offset_ptr&) noexcept = default;
    offset_ptr(offset_ptr&&) noexcept = default;
    offset_ptr& operator=(const offset_ptr&) noexcept = default;
    offset_ptr& operator=(offset_ptr&&) noexcept = default;
    ~offset_ptr() = default;

    template <class U>
    requires (std::is_convertible_v<U*, T*>)
    SHM_FORCE_INLINE offset_ptr(const offset_ptr<U, self_reloc_anchor, OffsetT>& other) noexcept
        : off_plus1_(other.raw_storage()) {}

    SHM_FORCE_INLINE offset_ptr& operator=(pointer p) noexcept { set(p); return *this; }
    SHM_FORCE_INLINE offset_ptr& operator=(std::nullptr_t) noexcept { off_plus1_ = 0; return *this; }

    [[nodiscard]] SHM_FORCE_INLINE pointer get() const noexcept {
        const offset_type s = off_plus1_;
        if (SHM_UNLIKELY(s == 0)) return nullptr;

        const detail::uptr b = self_reloc_anchor::base(this);

        if constexpr (std::is_signed_v<offset_type>) {
            const detail::iptr off = static_cast<detail::iptr>(s) - 1;
            return reinterpret_cast<pointer>(b + static_cast<detail::uptr>(off));
        } else {
            const detail::uptr off = static_cast<detail::uptr>(s - 1);
            return reinterpret_cast<pointer>(b + off);
        }
    }

    [[nodiscard]] SHM_FORCE_INLINE offset_type raw_storage() const noexcept { return off_plus1_; }
    [[nodiscard]] SHM_FORCE_INLINE explicit operator bool() const noexcept { return off_plus1_ != 0; }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U& operator*() const noexcept { return *get(); }

    template <class U = T>
    requires (!std::is_void_v<U>)
    [[nodiscard]] SHM_FORCE_INLINE U* operator->() const noexcept { return get(); }

private:
    SHM_FORCE_INLINE void set(pointer p) noexcept {
        if (!p) { off_plus1_ = 0; return; }

        const detail::uptr b = self_reloc_anchor::base(this);
        const detail::uptr t = detail::addr(p);
        const detail::iptr diff = static_cast<detail::iptr>(t) - static_cast<detail::iptr>(b);

        if constexpr (std::is_signed_v<offset_type>) {
            SHM_ASSERT(diff != -1 && "diff == -1 would encode to 0 (reserved for null).");
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        } else {
            SHM_ASSERT(diff >= 0);
            off_plus1_ = detail::narrow_checked<offset_type>(diff + 1);
        }
    }

    offset_type off_plus1_ = 0;
};

template <class T, class Tag, detail::offset_int OffsetT = std::uint32_t>
using segment_offset_ptr = offset_ptr<T, segment_anchor<Tag>, OffsetT>;
template <class T, detail::offset_int OffsetT = std::int32_t>
using self_reloc_ptr = offset_ptr<T, self_reloc_anchor, OffsetT>;



template <class T1, class A1, detail::offset_int O1,
          class T2, class A2, detail::offset_int O2>
requires (detail::is_obj_or_void_v<T1> && detail::is_obj_or_void_v<T2>)
SHM_FORCE_INLINE bool operator==(const offset_ptr<T1, A1, O1>& a,
                                 const offset_ptr<T2, A2, O2>& b) noexcept {
    return static_cast<const void*>(a.get()) == static_cast<const void*>(b.get());
}

template <class T1, class A1, detail::offset_int O1,
          class T2, class A2, detail::offset_int O2>
requires (detail::is_obj_or_void_v<T1> && detail::is_obj_or_void_v<T2>)
SHM_FORCE_INLINE bool operator!=(const offset_ptr<T1, A1, O1>& a,
                                 const offset_ptr<T2, A2, O2>& b) noexcept {
    return !(a == b);
}

template <class T, class A, detail::offset_int O>
requires (detail::is_obj_or_void_v<T>)
SHM_FORCE_INLINE bool operator==(const offset_ptr<T, A, O>& a, std::nullptr_t) noexcept {
    return a.get() == nullptr;
}

template <class T, class A, detail::offset_int O>
requires (detail::is_obj_or_void_v<T>)
SHM_FORCE_INLINE bool operator==(std::nullptr_t, const offset_ptr<T, A, O>& a) noexcept {
    return a.get() == nullptr;
}

template <class T, class A, detail::offset_int O>
requires (detail::is_obj_or_void_v<T>)
SHM_FORCE_INLINE bool operator!=(const offset_ptr<T, A, O>& a, std::nullptr_t) noexcept {
    return !(a == nullptr);
}

template <class T, class A, detail::offset_int O>
requires (detail::is_obj_or_void_v<T>)
SHM_FORCE_INLINE bool operator!=(std::nullptr_t, const offset_ptr<T, A, O>& a) noexcept {
    return !(a == nullptr);
}


template <class Tag, detail::offset_int OffsetT = std::uint32_t>
class linear_allocator {
public:
    using tag_type    = Tag;
    using offset_type = OffsetT;

    template <class T>
    using handle = shm::segment_offset_ptr<T, Tag, OffsetT>;

    using void_handle = handle<void>;

    linear_allocator(void* start, std::size_t size) noexcept
        : linear_allocator(start, start, size)
    {}

    linear_allocator(void* segment_base, void* arena_start, std::size_t arena_size) noexcept
        : arena_(reinterpret_cast<std::byte*>(arena_start))
        , arena_addr_(reinterpret_cast<std::uintptr_t>(arena_start))
        , capacity_(arena_size)
        , cursor_(0)
    {

        shm::segment_base<Tag>::set(segment_base);
    }

    linear_allocator(const linear_allocator&) = delete;
    linear_allocator& operator=(const linear_allocator&) = delete;
    linear_allocator(linear_allocator&&) = delete;
    linear_allocator& operator=(linear_allocator&&) = delete;

    [[nodiscard]] void* alloc(std::size_t n,
                              std::size_t alignment = alignof(std::max_align_t)) noexcept
    {
        if (n == 0) return nullptr;
        if (alignment == 0) alignment = 1;

        const bool pow2 = (alignment & (alignment - 1)) == 0;

        std::size_t cur = cursor_.load(std::memory_order_relaxed);
        for (;;) {
            if (cur > capacity_) return nullptr;

            const std::uintptr_t addr = arena_addr_ + static_cast<std::uintptr_t>(cur);
            std::uintptr_t aligned_addr;

            if (pow2) {
                const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1);
                aligned_addr = (addr + mask) & ~mask;
            } else {
                const std::uintptr_t a = static_cast<std::uintptr_t>(alignment);
                const std::uintptr_t rem = addr % a;
                aligned_addr = (rem == 0) ? addr : (addr + (a - rem));
            }

            const std::uintptr_t aligned_off_u = aligned_addr - arena_addr_;
            if (aligned_off_u > static_cast<std::uintptr_t>(capacity_)) return nullptr;

            const std::size_t aligned_off = static_cast<std::size_t>(aligned_off_u);
            if (n > capacity_ - aligned_off) return nullptr;

            const std::size_t next = aligned_off + n;

            if (cursor_.compare_exchange_weak(
                    cur, next,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return arena_ + aligned_off;
            }
        }
    }

    [[nodiscard]] void_handle alloc_handle(std::size_t n,
                                           std::size_t alignment = alignof(std::max_align_t)) noexcept
    {
        void* p = alloc(n, alignment);
        if (!p) return void_handle(nullptr);
        return void_handle(static_cast<void*>(p));
    }

    template <class T>
    [[nodiscard]] T* allocate(std::size_t count = 1) noexcept {
        static_assert(!std::is_void_v<T>, "allocate<void> is not meaningful.");
        if (count == 0) return nullptr;
        if (count > (std::numeric_limits<std::size_t>::max() / sizeof(T))) return nullptr;
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    template <class T>
    [[nodiscard]] handle<T> allocate_handle(std::size_t count = 1) noexcept {
        T* p = allocate<T>(count);
        if (!p) return handle<T>(nullptr);
        return handle<T>(p);
    }

    template <class T, class... Args>
    [[nodiscard]] handle<T> make_handle(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem) return handle<T>(nullptr);
        T* obj = ::new (mem) T(std::forward<Args>(args)...);
        return handle<T>(obj);
    }

    void reset() noexcept {
        cursor_.store(0, std::memory_order_release);
    }

    void secure_reset() noexcept {
        const std::size_t u = used();
        if (u) std::memset(arena_, 0, u);
        cursor_.store(0, std::memory_order_release);
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return cursor_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool owns(const void* p) const noexcept {
        const auto x = reinterpret_cast<std::uintptr_t>(p);
        return x >= arena_addr_ && x < (arena_addr_ + static_cast<std::uintptr_t>(capacity_));
    }

    
    template <class T>
    struct stl_allocator {
        using value_type = T;

        linear_allocator* arena = nullptr;

        stl_allocator() noexcept = default;
        explicit stl_allocator(linear_allocator& a) noexcept : arena(&a) {}

        template <class U>
        stl_allocator(const stl_allocator<U>& other) noexcept : arena(other.arena) {}

        [[nodiscard]] T* allocate(std::size_t n) {
            if (!arena) throw std::bad_alloc();
            if (n > (std::numeric_limits<std::size_t>::max() / sizeof(T))) throw std::bad_alloc();
            void* p = arena->alloc(sizeof(T) * n, alignof(T));
            if (!p) throw std::bad_alloc();
            return static_cast<T*>(p);
        }

        void deallocate(T*, std::size_t) noexcept {
        }

        template <class U>
        struct rebind { using other = stl_allocator<U>; };

        using propagate_on_container_copy_assignment = std::true_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap            = std::true_type;
        using is_always_equal                        = std::false_type;

        template <class U>
        friend bool operator==(const stl_allocator& a, const stl_allocator<U>& b) noexcept {
            return a.arena == b.arena;
        }
        template <class U>
        friend bool operator!=(const stl_allocator& a, const stl_allocator<U>& b) noexcept {
            return !(a == b);
        }
    };

private:
    std::byte* const arena_;
    std::uintptr_t const arena_addr_;
    std::size_t const capacity_;
    std::atomic<std::size_t> cursor_;
};


#include "shmTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif


namespace detail {

#if !defined(_WIN32)

template <class Fn, class... Args>
static inline auto retry_eintr_call_(Fn fn, Args... args) noexcept -> decltype(fn(args...)) {
    for (;;) {
        auto rc = fn(args...);
        if (rc == decltype(rc)(-1) && errno == EINTR) continue;
        return rc;
    }
}

static inline std::size_t page_size_() noexcept {
    long ps = ::sysconf(_SC_PAGESIZE);
    if (ps <= 0) return 4096;
    return static_cast<std::size_t>(ps);
}

static inline std::size_t round_up_(std::size_t x, std::size_t a) noexcept {
    if (a == 0) return x;
    const std::size_t r = x % a;
    return r == 0 ? x : (x + (a - r));
}

// POSIX shm name rules in practice must start with '/', must not contain any other '/'.
// SInce platforms are more permissive, we enforce the strict portable subset.
static inline bool name_is_portable_(std::string_view s) noexcept {
    if (s.empty()) return false;
    if (s[0] != '/') return false;
    if (s.size() == 1) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '/') return false;
        if (c == '\0') return false;
    }
    return true;
}

static inline std::string normalize_name_(const char* name) {
    if (!name || !*name) return {};
    std::string s(name);
    if (s[0] != '/') s.insert(s.begin(), '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

static inline void nanosleep_backoff_(std::size_t attempt) noexcept {
    // attempt=0.. => 100us, 200us, 400us ... capped at 10ms
    const std::size_t max_ns = 10ull * 1000ull * 1000ull;
    std::size_t ns = 100ull * 1000ull;
    if (attempt < 10) ns <<= attempt;
    if (ns > max_ns) ns = max_ns;
    timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(ns);
    ::nanosleep(&ts, nullptr);
}

#endif

} // namespace detail

class segment {
public:
    enum class open_mode {
        create_only,
        open_only,
        open_or_create
    };

    segment(const char* name, std::size_t size, open_mode mode)
#if defined(_WIN32)
        : base_(nullptr)
        , size_(0)
        , map_size_(0)
        , valid_(false)
        , created_(false)
        , name_()
#else
        : fd_(-1)
        , base_(nullptr)
        , size_(0)
        , map_size_(0)
        , created_(false)
        , name_(detail::normalize_name_(name))
#endif
    {
#if defined(_WIN32)
        (void)name;
        (void)size;
        (void)mode;
        valid_ = false;
#else
        if (name_.empty()) {
            throw std::invalid_argument("shm::segment: name must not be null/empty");
        }
        if (!detail::name_is_portable_(name_)) {
            throw std::invalid_argument("shm::segment: name must be portable POSIX shm form '/X' with no additional '/'");
        }

        const bool may_create = (mode == open_mode::create_only || mode == open_mode::open_or_create);
        if (may_create && size == 0) {
            throw std::invalid_argument("shm::segment: size must be > 0 for create modes");
        }

        const std::size_t ps = detail::page_size_();

        const int perms = 0600;

        int flags = O_RDWR;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif

        auto throw_errno = [&](const char* op) -> void {
            const int e = errno;
            std::error_code ec(e, std::generic_category());
            std::string msg;
            msg.reserve(256);
            msg.append("shm::segment: ");
            msg.append(op);
            msg.append(" failed (name=");
            msg.append(name_);
            msg.append(", errno=");
            msg.append(std::to_string(e));
            msg.append(", ");
            msg.append(ec.message());
            msg.append(")");
            throw std::system_error(ec, msg);
        };

        auto close_noexcept = [&](int& fd) noexcept {
            if (fd != -1) {
                (void)detail::retry_eintr_call_(::close, fd);
                fd = -1;
            }
        };

        auto unlink_noexcept = [&]() noexcept {
            if (!name_.empty()) {
                (void)::shm_unlink(name_.c_str());
            }
        };

        auto fail_ctor = [&](bool unlink_if_created) -> void {
            if (base_) {
                ::munmap(base_, map_size_);
                base_ = nullptr;
                size_ = 0;
                map_size_ = 0;
            }
            close_noexcept(fd_);
            if (unlink_if_created) unlink_noexcept();
        };

        int fd = -1;
        if (mode == open_mode::create_only) {
            fd = ::shm_open(name_.c_str(), flags | O_CREAT | O_EXCL, perms);
            if (fd == -1) throw_errno("shm_open(create_only)");
            created_ = true;
        } else if (mode == open_mode::open_only) {
            fd = ::shm_open(name_.c_str(), flags, perms);
            if (fd == -1) throw_errno("shm_open(open_only)");
            created_ = false;
        } else {
            fd = ::shm_open(name_.c_str(), flags | O_CREAT | O_EXCL, perms);
            if (fd == -1) {
                if (errno != EEXIST) throw_errno("shm_open(open_or_create/create)");
                fd = ::shm_open(name_.c_str(), flags, perms);
                if (fd == -1) throw_errno("shm_open(open_or_create/open)");
                created_ = false;
            } else {
                created_ = true;
            }
        }

        fd_ = fd;

        std::size_t seg_size = 0;
        if (created_) {
            if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
                const int saved = errno;
                fail_ctor(true);
                errno = saved;
                throw_errno("ftruncate(create)");
            }
            seg_size = size;
        } else {
            struct stat st {};
            bool ok = false;

            for (std::size_t attempt = 0; attempt < 200; ++attempt) {
                if (::fstat(fd_, &st) != 0) {
                    const int saved = errno;
                    fail_ctor(false);
                    errno = saved;
                    throw_errno("fstat(open)");
                }
                if (st.st_size > 0) {
                    ok = true;
                    break;
                }
                detail::nanosleep_backoff_(attempt);
            }

            if (!ok) {
                fail_ctor(false);
                throw std::runtime_error("shm::segment: existing segment reports size 0 after retries");
            }

            const std::size_t existing = static_cast<std::size_t>(st.st_size);

            if (size != 0 && existing < size) {
                fail_ctor(false);
                throw std::runtime_error("shm::segment: existing segment smaller than requested size");
            }

            seg_size = existing;
        }

        size_ = seg_size;
        map_size_ = detail::round_up_(seg_size, ps);

        void* map = ::mmap(nullptr,
                           map_size_,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           fd_,
                           0);

        if (map == MAP_FAILED) {
            const int saved = errno;
            fail_ctor(created_);
            errno = saved;
            throw_errno("mmap");
        }

        base_ = map;

#if defined(MADV_DONTDUMP)
        (void)::madvise(base_, map_size_, MADV_DONTDUMP);
#endif
#if defined(MADV_HUGEPAGE)
        (void)::madvise(base_, map_size_, MADV_HUGEPAGE);
#endif

        if (created_) {
            std::memset(base_, 0, size_);
        }
#endif
    }

    ~segment() noexcept {
#if defined(_WIN32)
        base_ = nullptr;
        size_ = 0;
        map_size_ = 0;
        valid_ = false;
        created_ = false;
#else
        if (base_) {
            ::munmap(base_, map_size_);
            base_ = nullptr;
        }
        if (fd_ != -1) {
            (void)detail::retry_eintr_call_(::close, fd_);
            fd_ = -1;
        }
        size_ = 0;
        map_size_ = 0;
        created_ = false;
#endif
    }

    segment(const segment&) = delete;
    segment& operator=(const segment&) = delete;
    segment(segment&&) = delete;
    segment& operator=(segment&&) = delete;

    void* base() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }

    bool is_valid() const noexcept {
#if defined(_WIN32)
        return valid_;
#else
        return fd_ != -1 && base_ != nullptr && size_ != 0;
#endif
    }

    static bool remove(const char* name) noexcept {
#if defined(_WIN32)
        (void)name;
        return false;
#else
        const std::string n = detail::normalize_name_(name);
        if (n.empty()) return false;
        if (!detail::name_is_portable_(n)) return false;

        if (::shm_unlink(n.c_str()) == 0) return true;
        if (errno == ENOENT) return true;
        return false;
#endif
    }

    template <class Tag>
    void bind() const noexcept {
        if (base_) {
            shm::segment_base<Tag>::set(base_);
        }
    }

    template <class Tag, detail::offset_int OffsetT = std::uint32_t>
    [[nodiscard]] shm::linear_allocator<Tag, OffsetT> make_allocator(std::size_t arena_offset = 0,
                                                                     std::size_t arena_size = 0) const noexcept
    {
        if (!base_ || size_ == 0 || arena_offset > size_) {
            return shm::linear_allocator<Tag, OffsetT>(nullptr, nullptr, 0);
        }

        std::byte* seg_base = reinterpret_cast<std::byte*>(base_);
        std::byte* arena = seg_base + arena_offset;

        std::size_t avail = size_ - arena_offset;
        if (arena_size != 0 && arena_size < avail) avail = arena_size;

        return shm::linear_allocator<Tag, OffsetT>(seg_base, arena, avail);
    }

private:
#if defined(_WIN32)
    void* base_;
    std::size_t size_;
    std::size_t map_size_;
    bool valid_;
    bool created_;
    std::string name_;
#else
    int fd_;
    void* base_;
    std::size_t size_;
    std::size_t map_size_;
    bool created_;
    std::string name_;
#endif
};


} // namespace shm
