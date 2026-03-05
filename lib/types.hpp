#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocazam {
    //

    namespace detail {

        inline constexpr size_t round_to_multiple_of(size_t value, size_t multiple) noexcept {
            size_t rem = value % multiple;
            if (rem == 0) {
                return value;
            }

            return value + (multiple - rem);
        }

#if defined(__cpp_lib_hardware_interference_size)
        inline constexpr size_t cache_line_size = [] {
            size_t s = std::hardware_destructive_interference_size;
            return std::has_single_bit(s) ? s : size_t{64};
        }();
#else
        inline constexpr size_t cache_line_size = 64;
#endif
    }  // namespace detail

    template <typename T>
    struct node_t {
        static constexpr size_t node_size{std::ranges::max(sizeof(T), sizeof(void*))};
        static constexpr size_t node_alignment{std::ranges::max(alignof(T), alignof(void*))};

        alignas(node_alignment) std::array<std::byte, node_size> storage;
    };

    template <typename T, bool owns_memory = true>
    struct chunk_t {
        using node = node_t<T>;

        constexpr chunk_t() = default;

        constexpr explicit chunk_t(size_t sz, size_t page_size, size_t alloc_alignment)
            requires(owns_memory)
                : count{sz}, alloc_align{alloc_alignment} {
            void* raw = ::operator new[](
                    detail::round_to_multiple_of(count * sizeof(node), page_size), std::align_val_t{alloc_align});
            nodes = static_cast<node*>(raw);
            std::uninitialized_default_construct_n(nodes, count);
        }

        constexpr explicit chunk_t(node* external_nodes, size_t sz)
            requires(!owns_memory)
                : count{sz}, nodes{external_nodes} {}

        constexpr ~chunk_t() { release(); }

        chunk_t(const chunk_t&) = delete;
        chunk_t& operator=(const chunk_t&) = delete;

        constexpr chunk_t(chunk_t&& other) noexcept
                : count(std::exchange(other.count, 0)),
                  alloc_align(std::exchange(other.alloc_align, alignof(node))),
                  nodes(std::exchange(other.nodes, nullptr)) {}

        constexpr chunk_t& operator=(chunk_t&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();
            count = std::exchange(other.count, 0);
            alloc_align = std::exchange(other.alloc_align, alignof(node));
            nodes = std::exchange(other.nodes, nullptr);
            return *this;
        }

        constexpr auto at(this auto&& self, size_t i) noexcept {
            using self_t = std::remove_reference_t<decltype(self)>;
            using ptr_t = std::conditional_t<std::is_const_v<self_t>, const node*, node*>;
            return static_cast<ptr_t>(self.nodes + i);
        }

        constexpr void release() noexcept {
            if (nodes == nullptr) {
                return;
            }
            if constexpr (owns_memory) {
                if constexpr (!std::is_trivially_destructible_v<node>) {
                    std::destroy_n(nodes, count);
                }
                ::operator delete[](nodes, std::align_val_t{alloc_align});
            }
            nodes = nullptr;
            count = 0;
        }

        size_t count{};
        size_t alloc_align{alignof(node)};
        node* nodes{nullptr};
    };

}  // namespace allocazam
