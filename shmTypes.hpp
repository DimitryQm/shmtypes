#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <limits>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
  #define SHM_FORCE_INLINE inline __attribute__((always_inline))
  #define SHM_LIKELY(x)   (__builtin_expect(!!(x), 1))
  #define SHM_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
  #define SHM_FORCE_INLINE inline
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
    concept signed_integral = std::is_integral_v<I> && std::is_signed_v<I>;

    template <signed_integral To, class From>
    SHM_FORCE_INLINE constexpr To narrow_cast_checked(From v) noexcept {
        if constexpr (std::is_signed_v<From>) {
            SHM_ASSERT(v >= static_cast<From>(std::numeric_limits<To>::min()));
            SHM_ASSERT(v <= static_cast<From>(std::numeric_limits<To>::max()));
        } else {
            SHM_ASSERT(v <= static_cast<From>(std::numeric_limits<To>::max()));
        }
        return static_cast<To>(v);
    }

    SHM_FORCE_INLINE constexpr uptr addr_of(const void* p) noexcept {
        return reinterpret_cast<uptr>(p);
    }

    SHM_FORCE_INLINE constexpr void* ptr_of(uptr a) noexcept {
        return reinterpret_cast<void*>(a);
    }
} // namespace detail

template <class Tag>
struct segment_base {
    static SHM_FORCE_INLINE void set(void* base) noexcept {
        base_ = reinterpret_cast<std::byte*>(base);
    }

    static SHM_FORCE_INLINE std::byte* get() noexcept {
        return base_;
    }

private:
    inline static std::byte* base_ = nullptr;
};

struct self_anchor {
    static constexpr bool kSelfRelative = true;

    static SHM_FORCE_INLINE detail::uptr base(const void* self) noexcept {
        return detail::addr_of(self);
    }
};

template <class Tag>
struct segment_anchor {
    static constexpr bool kSelfRelative = false;

    static SHM_FORCE_INLINE detail::uptr base(const void*) noexcept {
        auto* b = segment_base<Tag>::get();
        SHM_ASSERT(b != nullptr && "segment_base<Tag>::set(base) must be called before using segment_anchor<Tag>.");
        return detail::addr_of(b);
    }
};

template <class T,
          class Anchor = self_anchor,
          detail::signed_integral OffsetT = std::int32_t>
class offset_ptr {
public:
    using element_type    = T;
    using value_type      = std::remove_cv_t<T>;
    using pointer         = T*;
    using reference       = T&;
    using difference_type = std::ptrdiff_t;
    using offset_type     = OffsetT;

    static_assert(std::is_object_v<T> || std::is_void_v<T>,
                  "offset_ptr<T>: T must be an object type or void.");
    static_assert(sizeof(offset_type) <= sizeof(detail::iptr),
                  "OffsetT too wide for pointer-sized arithmetic.");
    constexpr offset_ptr() noexcept = default;
    constexpr offset_ptr(std::nullptr_t) noexcept : off_plus1_(0) {}

    SHM_FORCE_INLINE explicit offset_ptr(pointer p) noexcept { set(p); }

    template <class U>
    requires (std::is_convertible_v<U*, T*>)
    SHM_FORCE_INLINE offset_ptr(const offset_ptr<U, Anchor, OffsetT>& other) noexcept {
        if constexpr (Anchor::kSelfRelative) {
            set(other.get());
        } else {
            off_plus1_ = other.raw_storage();
        }
    }

    SHM_FORCE_INLINE offset_ptr(const offset_ptr& other) noexcept {
        if constexpr (Anchor::kSelfRelative) {
            set(other.get());
        } else {
            off_plus1_ = other.off_plus1_;
        }
    }

    SHM_FORCE_INLINE offset_ptr& operator=(const offset_ptr& other) noexcept {
        if (this == &other) return *this;
        if constexpr (Anchor::kSelfRelative) {
            set(other.get());
        } else {
            off_plus1_ = other.off_plus1_;
        }
        return *this;
    }

    SHM_FORCE_INLINE offset_ptr& operator=(std::nullptr_t) noexcept {
        off_plus1_ = 0;
        return *this;
    }

    SHM_FORCE_INLINE offset_ptr& operator=(pointer p) noexcept {
        set(p);
        return *this;
    }

    [[nodiscard]] SHM_FORCE_INLINE pointer get() const noexcept {
        const offset_type s = off_plus1_;
        if (SHM_UNLIKELY(s == 0)) return nullptr;

        const auto b = Anchor::base(this);
        const auto off = static_cast<detail::iptr>(s) - 1; // signed
        return reinterpret_cast<pointer>(detail::ptr_of(b + static_cast<detail::uptr>(off)));
    }

    SHM_FORCE_INLINE void reset() noexcept { off_plus1_ = 0; }

    [[nodiscard]] SHM_FORCE_INLINE explicit operator bool() const noexcept { return off_plus1_ != 0; }

    [[nodiscard]] SHM_FORCE_INLINE reference operator*() const {
        return *get();
    }

    [[nodiscard]] SHM_FORCE_INLINE pointer operator->() const noexcept requires (!std::is_void_v<T>) {
        return get();
    }

    [[nodiscard]] SHM_FORCE_INLINE T& operator[](difference_type i) const requires (!std::is_void_v<T>) {
        return get()[i];
    }

    friend SHM_FORCE_INLINE bool operator==(const offset_ptr& a, std::nullptr_t) noexcept { return !a; }
    friend SHM_FORCE_INLINE bool operator==(std::nullptr_t, const offset_ptr& a) noexcept { return !a; }

    friend SHM_FORCE_INLINE bool operator==(const offset_ptr& a, const offset_ptr& b) noexcept {
        return a.get() == b.get();
    }

    [[nodiscard]] SHM_FORCE_INLINE offset_type raw_storage() const noexcept { return off_plus1_; }

private:
    SHM_FORCE_INLINE void set(pointer p) noexcept {
        if (p == nullptr) {
            off_plus1_ = 0;
            return;
        }

        const auto b = Anchor::base(this);
        const auto t = detail::addr_of(p);
        const auto off_ip = static_cast<detail::iptr>(t) - static_cast<detail::iptr>(b);
        const auto enc = off_ip + 1;
        SHM_ASSERT(enc != 0 && "Encoded offset overflowed to 0 (range issue).");

        off_plus1_ = detail::narrow_cast_checked<offset_type>(enc);
    }

    offset_type off_plus1_ = 0; // 0 => null, else (offset + 1)
};

template <class T, class Tag, detail::signed_integral OffsetT = std::int32_t>
using segment_offset_ptr = offset_ptr<T, segment_anchor<Tag>, OffsetT>;

} // namespace shm
