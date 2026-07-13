#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace allocazam::detail {
    class exclusive_resource {
      public:
        exclusive_resource(std::byte* base, size_t usable_bytes, size_t effective_alignment)
                : _base{base}, _usable_bytes{usable_bytes}, _effective_alignment{effective_alignment} {
            if (_base == nullptr) {
                throw std::invalid_argument{"exclusive resource base must not be null"};
            }
            if (_usable_bytes == 0) {
                throw std::invalid_argument{"exclusive resource capacity must not be zero"};
            }
            if (_effective_alignment == 0 || !std::has_single_bit(_effective_alignment)) {
                throw std::invalid_argument{"exclusive resource alignment must be a power of two"};
            }
            if (!_is_aligned(_base, _effective_alignment)) {
                throw std::invalid_argument{"exclusive resource base does not satisfy its alignment"};
            }
        }

        ~exclusive_resource() { assert(!claimed() && "exclusive resource destroyed with a live claim"); }

        exclusive_resource(const exclusive_resource&) = delete;
        exclusive_resource& operator=(const exclusive_resource&) = delete;
        exclusive_resource(exclusive_resource&&) = delete;
        exclusive_resource& operator=(exclusive_resource&&) = delete;

        [[nodiscard]] void* claim(size_t minimum_bytes, size_t maximum_bytes, size_t alignment) noexcept {
            if (claimed() || minimum_bytes == 0 || minimum_bytes > maximum_bytes || maximum_bytes > _usable_bytes ||
                alignment == 0 || !std::has_single_bit(alignment) || alignment > _effective_alignment ||
                !_is_aligned(_base, alignment)) {
                return nullptr;
            }

            _claimed_min_bytes = minimum_bytes;
            _claimed_max_bytes = maximum_bytes;
            _claimed_alignment = alignment;
            return _base;
        }

        void release(void* pointer, size_t bytes, size_t alignment) noexcept {
            bool valid = claimed() && pointer == _base && _claimed_min_bytes <= bytes && bytes <= _claimed_max_bytes &&
                         alignment == _claimed_alignment;
            if (!valid) {
                assert(valid && "invalid exclusive resource release");
                return;
            }

            _claimed_min_bytes = 0;
            _claimed_max_bytes = 0;
            _claimed_alignment = 0;
        }

        [[nodiscard]] size_t expand(
                void* pointer, size_t minimum_new_bytes, size_t representable_capacity_bytes) noexcept {
            if (!claimed() || pointer != _base) {
                return 0;
            }
            if (minimum_new_bytes <= _claimed_max_bytes) {
                return _claimed_max_bytes;
            }
            if (representable_capacity_bytes < _claimed_max_bytes || representable_capacity_bytes > _usable_bytes ||
                minimum_new_bytes > representable_capacity_bytes) {
                return _claimed_max_bytes;
            }

            _claimed_max_bytes = representable_capacity_bytes;
            return _claimed_max_bytes;
        }

        [[nodiscard]] bool owns(const void* pointer) const noexcept { return pointer == _base; }
        [[nodiscard]] bool claimed() const noexcept { return _claimed_min_bytes != 0; }
        [[nodiscard]] size_t capacity_bytes() const noexcept { return _usable_bytes; }
        [[nodiscard]] size_t alignment() const noexcept { return _effective_alignment; }

      private:
        [[nodiscard]] static bool _is_aligned(const void* pointer, size_t alignment) noexcept {
            return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
        }

        std::byte* _base;
        size_t _usable_bytes;
        size_t _effective_alignment;

        size_t _claimed_min_bytes{};
        size_t _claimed_max_bytes{};
        size_t _claimed_alignment{};
    };
}  // namespace allocazam::detail
