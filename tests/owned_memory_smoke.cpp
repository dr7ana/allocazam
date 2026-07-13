#include "owned_memory.hpp"
#include "utils.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {
    using allocazam::detail::allocate_owned_memory;
    using allocazam::detail::explicit_huge_page_bytes;
    using allocazam::detail::release_owned_memory;

    void test_invalid_requests() {
        require(!allocate_owned_memory<allocazam::huge_pages::disabled>(0, alignof(std::max_align_t)),
                "zero-byte owned allocation must fail");
        require(!allocate_owned_memory<allocazam::huge_pages::disabled>(64, 3),
                "non-power-of-two owned alignment must fail");
        require(!allocate_owned_memory<allocazam::huge_pages::disabled>(64, alignof(void*) / size_t{2}),
                "sub-pointer owned alignment must fail");
    }

    void test_ordinary_exact_size_and_alignment() {
        constexpr size_t bytes = 65537;
        constexpr size_t alignment = 256;
        auto memory = allocate_owned_memory<allocazam::huge_pages::disabled>(bytes, alignment);
        require(memory.has_value(), "ordinary owned allocation failed");
        require(memory->base != nullptr, "ordinary owned allocation returned null");
        require(memory->bytes == bytes, "ordinary owned allocation changed requested bytes");
        require(memory->alignment == alignment, "ordinary owned allocation changed alignment contract");
        require((reinterpret_cast<uintptr_t>(memory->base) % alignment) == 0,
                "ordinary owned allocation is misaligned");

        memory->base[0] = std::byte{0x5a};
        memory->base[bytes - 1] = std::byte{0xa5};
        require(memory->base[0] == std::byte{0x5a}, "ordinary owned allocation first byte is unwritable");
        require(memory->base[bytes - 1] == std::byte{0xa5}, "ordinary owned allocation last byte is unwritable");

        release_owned_memory<allocazam::huge_pages::disabled>(*memory);
    }

#if defined(__linux__)
    void test_hugetlb_rounding_and_alignment() {
        auto exact = allocate_owned_memory<allocazam::huge_pages::enabled>(
                explicit_huge_page_bytes, alignof(std::max_align_t));
        require(exact.has_value(), "exact-page hugetlb allocation failed");
        require(exact->bytes == explicit_huge_page_bytes,
                "exact-page hugetlb allocation rounded to more than one page");
        require(exact->alignment == explicit_huge_page_bytes, "hugetlb effective alignment is not one huge page");
        require((reinterpret_cast<uintptr_t>(exact->base) % explicit_huge_page_bytes) == 0,
                "hugetlb allocation base is not huge-page aligned");
        release_owned_memory<allocazam::huge_pages::enabled>(*exact);

        auto rounded = allocate_owned_memory<allocazam::huge_pages::enabled>(
                explicit_huge_page_bytes + 1, alignof(std::max_align_t));
        require(rounded.has_value(), "two-page hugetlb allocation failed");
        require(rounded->bytes == (2 * explicit_huge_page_bytes), "hugetlb allocation did not round to two pages");
        release_owned_memory<allocazam::huge_pages::enabled>(*rounded);

        auto excessive_alignment =
                allocate_owned_memory<allocazam::huge_pages::enabled>(1, 2 * explicit_huge_page_bytes);
        require(!excessive_alignment, "unsupported hugetlb alignment must fail");

        auto overflow = allocate_owned_memory<allocazam::huge_pages::enabled>(
                std::numeric_limits<size_t>::max(), alignof(std::max_align_t));
        require(!overflow, "hugetlb mapping-size overflow must fail");
    }
#endif
}  // namespace

int main() {
    try {
        test_invalid_requests();
        test_ordinary_exact_size_and_alignment();
#if defined(__linux__)
        test_hugetlb_rounding_and_alignment();
#endif
    } catch (const std::exception& e) {
        std::cerr << "owned memory smoke failed: " << e.what() << '\n';
        return 1;
    }

    std::cout << "owned memory smoke: all tests passed\n";
    return 0;
}
