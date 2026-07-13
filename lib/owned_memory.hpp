#pragma once

#include "types.hpp"

#include <bit>
#include <expected>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21U << MAP_HUGE_SHIFT)
#endif
#endif

namespace allocazam::detail {
    inline constexpr size_t explicit_huge_page_bytes{size_t{2} << 20};

    enum class owned_memory_error : uint8_t {
        out_of_memory,
        hugetlb_failed,
    };

    struct owned_memory {
        std::byte* base{nullptr};
        size_t bytes{0};
        size_t alignment{alignof(std::max_align_t)};
    };

    template <huge_pages Huge>
    [[nodiscard]] std::expected<owned_memory, owned_memory_error> allocate_owned_memory(
            size_t bytes, size_t alignment) noexcept {
        if (bytes == 0 || alignment < alignof(void*) || !std::has_single_bit(alignment)) {
            return std::unexpected{owned_memory_error::out_of_memory};
        }

        if constexpr (Huge == huge_pages::enabled) {
#if defined(__linux__)
            if (alignment > explicit_huge_page_bytes) {
                return std::unexpected{owned_memory_error::hugetlb_failed};
            }

            size_t mapping_bytes = 0;
            if (!checked_round_to_multiple_of(bytes, explicit_huge_page_bytes, mapping_bytes)) {
                return std::unexpected{owned_memory_error::out_of_memory};
            }

            void* mapping =
                    ::mmap(nullptr,
                           mapping_bytes,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                           -1,
                           0);
            if (mapping == MAP_FAILED) {
                return std::unexpected{owned_memory_error::hugetlb_failed};
            }
            return owned_memory{
                    .base = static_cast<std::byte*>(mapping),
                    .bytes = mapping_bytes,
                    .alignment = explicit_huge_page_bytes,
            };
#else
            (void)bytes;
            (void)alignment;
            return std::unexpected{owned_memory_error::hugetlb_failed};
#endif
        } else {
            auto* raw = static_cast<std::byte*>(::operator new[](bytes, std::align_val_t{alignment}, std::nothrow));
            if (raw == nullptr) {
                return std::unexpected{owned_memory_error::out_of_memory};
            }
            return owned_memory{
                    .base = raw,
                    .bytes = bytes,
                    .alignment = alignment,
            };
        }
    }

    template <huge_pages Huge>
    void release_owned_memory(owned_memory memory) noexcept {
        if (memory.base == nullptr) {
            return;
        }

        if constexpr (Huge == huge_pages::enabled) {
#if defined(__linux__)
            ::munmap(static_cast<void*>(memory.base), memory.bytes);
#else
            (void)memory;
#endif
        } else {
            ::operator delete[](memory.base, std::align_val_t{memory.alignment});
        }
    }

    template <huge_pages Huge>
    class owned_memory_owner {
      public:
        explicit owned_memory_owner(size_t bytes, size_t alignment) {
            auto memory = allocate_owned_memory<Huge>(bytes, alignment);
            if (!memory) {
                throw std::bad_alloc{};
            }
            _memory = *memory;
        }

        ~owned_memory_owner() { release_owned_memory<Huge>(_memory); }

        owned_memory_owner(const owned_memory_owner&) = delete;
        owned_memory_owner& operator=(const owned_memory_owner&) = delete;
        owned_memory_owner(owned_memory_owner&&) = delete;
        owned_memory_owner& operator=(owned_memory_owner&&) = delete;

        [[nodiscard]] std::byte* base() const noexcept { return _memory.base; }
        [[nodiscard]] size_t bytes() const noexcept { return _memory.bytes; }
        [[nodiscard]] size_t alignment() const noexcept { return _memory.alignment; }

      private:
        owned_memory _memory{};
    };
}  // namespace allocazam::detail
