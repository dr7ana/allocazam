#include "allocazam.hpp"
#include "mapping_info.hpp"
#include "utils.hpp"

#include <array>
#include <concepts>
#include <iostream>
#include <limits>
#include <list>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
    using allocazam::allocation_model;
    using allocazam::huge_pages;
    using allocazam::memory_mode;

    template <typename T, memory_mode Memory>
    using exclusive_state =
            allocazam::allocazam_std_state<T, Memory, allocation_model::exclusive, huge_pages::disabled>;

    template <typename T, memory_mode Memory>
    using exclusive_allocator =
            allocazam::allocazam_std_allocator<T, Memory, allocation_model::exclusive, huge_pages::disabled>;

    template <typename Allocator>
    concept exposes_state = requires(Allocator allocator) { allocator.state(); };

    template <typename Allocator>
    concept exposes_thread_state_registry = requires { Allocator::thread_state_registry(); };

    template <typename State>
    concept exposes_mapping_bytes = requires(const State& state) { state.mapping_bytes(); };

    template <typename State>
    concept exposes_pool_or_runner = requires(State& state) {
        state.pool;
        state.runs;
    };

    using fixed_state = exclusive_state<int, memory_mode::fixed>;
    using fixed_allocator = exclusive_allocator<int, memory_mode::fixed>;
    using noheap_state = exclusive_state<int, memory_mode::noheap>;
    using noheap_allocator = exclusive_allocator<int, memory_mode::noheap>;

    struct tracked_value {
        inline static size_t live_count{};

        int value;

        explicit tracked_value(int value) : value{value} { ++live_count; }
        ~tracked_value() { --live_count; }
    };

    struct alignas(64) aligned_value {
        size_t value;
    };

    static_assert(allocazam::supported_std_allocator_configuration<
                  memory_mode::fixed,
                  allocation_model::exclusive,
                  huge_pages::disabled>);
#if defined(__linux__)
    static_assert(allocazam::supported_std_allocator_configuration<
                  memory_mode::fixed,
                  allocation_model::exclusive,
                  huge_pages::enabled>);
#else
    static_assert(!allocazam::supported_std_allocator_configuration<
                  memory_mode::fixed,
                  allocation_model::exclusive,
                  huge_pages::enabled>);
#endif
    static_assert(allocazam::supported_std_allocator_configuration<
                  memory_mode::noheap,
                  allocation_model::exclusive,
                  huge_pages::disabled>);
    static_assert(!allocazam::supported_std_allocator_configuration<
                  memory_mode::dynamic,
                  allocation_model::exclusive,
                  huge_pages::disabled>);
    static_assert(!allocazam::supported_std_allocator_configuration<
                  memory_mode::noheap,
                  allocation_model::exclusive,
                  huge_pages::enabled>);

    static_assert(!std::default_initializable<fixed_allocator>);
    static_assert(!std::default_initializable<noheap_allocator>);
    static_assert(std::constructible_from<fixed_allocator, fixed_state&>);
    static_assert(std::constructible_from<noheap_state, std::span<std::byte>>);
    static_assert(std::constructible_from<noheap_allocator, noheap_state&>);
    static_assert(std::copy_constructible<fixed_allocator>);
    static_assert(std::is_nothrow_copy_constructible_v<fixed_allocator>);
    static_assert(std::is_nothrow_move_constructible_v<fixed_allocator>);
    static_assert(std::is_nothrow_copy_assignable_v<fixed_allocator>);
    static_assert(std::is_nothrow_move_assignable_v<fixed_allocator>);
    static_assert(sizeof(fixed_allocator) == sizeof(void*));
    static_assert(sizeof(noheap_allocator) == sizeof(void*));
    static_assert(!std::copy_constructible<fixed_state>);
    static_assert(!std::move_constructible<fixed_state>);
    static_assert(!std::copy_constructible<noheap_state>);
    static_assert(!std::move_constructible<noheap_state>);
    static_assert(!exposes_state<fixed_allocator>);
    static_assert(!exposes_thread_state_registry<fixed_allocator>);
    static_assert(exposes_mapping_bytes<fixed_state>);
    static_assert(!exposes_mapping_bytes<noheap_state>);
    static_assert(!exposes_pool_or_runner<fixed_state>);
    static_assert(!exposes_pool_or_runner<noheap_state>);
    static_assert(!fixed_allocator::propagate_on_container_copy_assignment::value);
    static_assert(fixed_allocator::propagate_on_container_move_assignment::value);
    static_assert(fixed_allocator::propagate_on_container_swap::value);
    static_assert(!fixed_allocator::is_always_equal::value);
    static_assert(std::same_as<
                  typename fixed_allocator::template rebind<long>::other,
                  exclusive_allocator<long, memory_mode::fixed>>);
    static_assert(std::same_as<
                  typename noheap_allocator::template rebind<long>::other,
                  exclusive_allocator<long, memory_mode::noheap>>);
    static_assert(std::constructible_from<allocazam::allocazam<int, memory_mode::fixed>, size_t>);

    template <typename Exception, typename Fn>
    void require_throws(Fn&& fn, std::string_view message) {
        bool threw = false;
        try {
            fn();
        } catch (const Exception&) {
            threw = true;
        }
        require(threw, message);
    }

    void test_fixed_state_and_exact_claim() {
        require_throws<std::invalid_argument>([] { fixed_state state{0}; }, "zero fixed capacity must fail");
        require_throws<std::bad_array_new_length>(
                [] { fixed_state state{std::numeric_limits<size_t>::max()}; },
                "overflowing fixed capacity must fail before allocation");

        fixed_state state{8};
        require(state.capacity() == 8, "fixed state changed element capacity");
        require(state.capacity_bytes() == 8 * sizeof(int), "fixed state changed usable bytes");
        require(state.mapping_bytes() == state.capacity_bytes(), "ordinary fixed mapping must be exact-sized");
        require(!state.claimed(), "new fixed state must be unclaimed");

        fixed_allocator allocator{state};
        require(allocator.max_size() == state.capacity(), "exclusive max_size must be resource-limited");
        require(allocator.allocate(0) == nullptr, "zero allocation must return null");
        int* pointer = allocator.allocate(3);
        require(pointer != nullptr, "exact exclusive allocation failed");
        require(state.claimed(), "exact exclusive allocation did not claim state");
        pointer[0] = 11;
        pointer[2] = 33;
        require(pointer[0] == 11 && pointer[2] == 33, "exclusive payload is not writable");

        require_throws<std::bad_alloc>([&] { (void)allocator.allocate(1); }, "second live claim must fail");
        allocator.deallocate(pointer, 3);
        require(!state.claimed(), "exact deallocation did not release state");

        pointer = allocator.allocate(state.capacity());
        require(pointer[0] == 11 && pointer[2] == 33, "state reuse unexpectedly cleared payload bytes");
        allocator.deallocate(pointer, state.capacity());
        require(!state.claimed(), "fixed state was not reusable after release");
    }

    void test_allocate_at_least_interval_and_expand() {
        fixed_state state{8};
        fixed_allocator allocator{state};

        auto empty = allocator.allocate_at_least(0);
        require(empty.ptr == nullptr && empty.count == 0, "zero allocate_at_least must return null/zero");
        require(!state.claimed(), "zero allocate_at_least claimed the state");

        for (size_t release_count : {size_t{3}, size_t{5}, state.capacity()}) {
            auto result = allocator.allocate_at_least(3);
            require(result.ptr != nullptr, "allocate_at_least returned null");
            require(result.count == state.capacity(), "allocate_at_least did not expose full capacity");
            allocator.deallocate(result.ptr, release_count);
            require(!state.claimed(), "legal allocate_at_least release count did not clear claim");
        }

        require_throws<std::bad_alloc>(
                [&] { (void)allocator.allocate_at_least(state.capacity() + 1); },
                "representable oversized request must throw bad_alloc");
        require_throws<std::bad_array_new_length>(
                [&] { (void)allocator.allocate(std::numeric_limits<size_t>::max()); },
                "arithmetic-overflow request must throw bad_array_new_length");
        require(allocator.expand(static_cast<int*>(nullptr), sizeof(int)) == 0, "null expand must return zero");
        int foreign = 0;
        require(allocator.expand(&foreign, sizeof(int)) == 0, "foreign expand must return zero");

        int* pointer = allocator.allocate(2);
        pointer[0] = 71;
        pointer[1] = 72;
        require(allocator.expand(pointer, sizeof(int)) == 2 * sizeof(int), "no-op expand changed exact claim");
        require(allocator.expand(pointer, state.capacity_bytes() + 1) == 2 * sizeof(int),
                "oversized expand changed exact claim");
        require(allocator.expand(pointer, 3 * sizeof(int)) == state.capacity_bytes(),
                "valid expand did not expose representable capacity");
        require(pointer[0] == 71 && pointer[1] == 72, "expand modified live payload");
        allocator.deallocate(pointer, state.capacity());
        require(!state.claimed(), "expanded claim was not releasable at full capacity");
    }

    void test_noheap_alignment_truncation_and_reuse() {
        std::array<std::byte, 129> backing{};
        backing.fill(std::byte{0x5a});
        require_throws<std::invalid_argument>(
                [] { noheap_state state{std::span<std::byte>{}}; }, "empty noheap backing must fail");
        require_throws<std::invalid_argument>(
                [] {
                    std::array<std::byte, sizeof(int) - 1> too_small{};
                    noheap_state state{too_small};
                },
                "insufficient noheap backing must fail");

        alignas(std::max_align_t) std::array<std::byte, 8 * sizeof(int)> aligned_backing{};
        {
            noheap_state aligned_state{aligned_backing};
            noheap_allocator aligned_allocator{aligned_state};
            int* aligned_pointer = aligned_allocator.allocate(aligned_state.capacity());
            require(reinterpret_cast<std::byte*>(aligned_pointer) == aligned_backing.data(),
                    "already-aligned noheap backing changed base");
            aligned_allocator.deallocate(aligned_pointer, aligned_state.capacity());
        }

        size_t capacity = 0;
        {
            noheap_state state{std::span<std::byte>{backing}.subspan(1)};
            capacity = state.capacity();
            require(capacity > 0, "noheap alignment discarded all capacity");
            require(state.capacity_bytes() == capacity * sizeof(int), "noheap bytes were not element-truncated");
            for (std::byte value : backing) {
                require(value == std::byte{0x5a}, "noheap state construction modified caller backing");
            }

            noheap_allocator allocator{state};
            int* pointer = allocator.allocate(capacity);
            auto* bytes = reinterpret_cast<std::byte*>(pointer);
            require((reinterpret_cast<uintptr_t>(pointer) % alignof(std::max_align_t)) == 0,
                    "noheap exclusive base did not receive deterministic alignment");
            require(bytes >= backing.data() && (bytes + state.capacity_bytes()) <= (backing.data() + backing.size()),
                    "noheap allocation escaped caller backing");
            pointer[0] = 1234;
            size_t prefix_bytes = static_cast<size_t>(bytes - backing.data());
            for (size_t i = 0; i < prefix_bytes; ++i) {
                require(backing[i] == std::byte{0x5a}, "noheap alignment prefix was modified");
            }
            size_t suffix_begin = prefix_bytes + state.capacity_bytes();
            for (size_t i = suffix_begin; i < backing.size(); ++i) {
                require(backing[i] == std::byte{0x5a}, "noheap truncated suffix was modified");
            }
            allocator.deallocate(pointer, capacity);
            require(!state.claimed(), "noheap state did not release exact claim");
        }

        backing.fill(std::byte{0xa5});
        for (std::byte value : backing) {
            require(value == std::byte{0xa5}, "caller could not reuse noheap backing after state destruction");
        }
    }

    void test_rebind_identity_and_alignment_rejection() {
        struct alignas(64) over_aligned {
            std::byte value;
        };

        fixed_state state{16};
        fixed_allocator allocator{state};
        using rebound_allocator = typename fixed_allocator::template rebind<over_aligned>::other;
        rebound_allocator rebound{allocator};
        require(rebound == allocator, "rebound exclusive allocator changed resource identity");
        require(rebound.max_size() == 0, "unsupported rebound alignment must report zero max_size");
        require_throws<std::bad_alloc>([&] { (void)rebound.allocate(1); }, "unsupported rebound alignment must fail");
        require(!state.claimed(), "failed rebound allocation claimed the state");

        using byte_state = exclusive_state<std::byte, memory_mode::fixed>;
        using byte_allocator = exclusive_allocator<std::byte, memory_mode::fixed>;
        byte_state compatible_state{64};
        byte_allocator bytes{compatible_state};
        using int_rebound = typename byte_allocator::template rebind<int>::other;
        int_rebound ints{bytes};
        require(ints == bytes && bytes == ints, "cross-type exclusive equality is not symmetric");
        int* values = ints.allocate(4);
        require_throws<std::bad_alloc>([&] { (void)bytes.allocate(1); }, "rebound claim escaped one-live limit");
        ints.deallocate(values, 4);
        require(!compatible_state.claimed(), "compatible rebound release did not clear claim");
    }

    void test_object_lifetime_alignment_and_allocator_handles() {
        using tracked_state = exclusive_state<tracked_value, memory_mode::fixed>;
        using tracked_allocator = exclusive_allocator<tracked_value, memory_mode::fixed>;
        tracked_state state{3};
        tracked_allocator allocator{state};

        tracked_allocator copied{allocator};
        tracked_allocator moved{std::move(copied)};
        require(copied == allocator && moved == allocator, "allocator move invalidated source identity");
        copied = moved;
        moved = std::move(copied);
        require(copied == allocator && moved == allocator, "allocator assignment changed resource identity");

        tracked_value* values = allocator.allocate(3);
        for (size_t i = 0; i < 3; ++i) {
            allocator.construct(values + i, static_cast<int>(100 + i));
        }
        require(tracked_value::live_count == 3, "exclusive construct hook missed objects");
        require(values[2].value == 102, "exclusive constructed value changed");
        for (size_t i = 0; i < 3; ++i) {
            allocator.destroy(values + i);
        }
        require(tracked_value::live_count == 0, "exclusive destroy hook missed objects");
        allocator.deallocate(values, 3);

        using aligned_state = exclusive_state<aligned_value, memory_mode::fixed>;
        using aligned_allocator = exclusive_allocator<aligned_value, memory_mode::fixed>;
        aligned_state aligned_storage{2};
        aligned_allocator aligned_alloc{aligned_storage};
        aligned_value* aligned = aligned_alloc.allocate(2);
        require((reinterpret_cast<uintptr_t>(aligned) % alignof(aligned_value)) == 0,
                "fixed exclusive state did not honor over-alignment");
        aligned_alloc.construct(aligned, aligned_value{77});
        require(aligned->value == 77, "over-aligned construction changed value");
        aligned_alloc.destroy(aligned);
        aligned_alloc.deallocate(aligned, 2);
    }

    template <typename Allocator>
    using int_vector = std::vector<int, Allocator>;

    void fill_reserved_vector(auto& values, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            values.push_back(static_cast<int>(i * 7));
        }
    }

    void test_fixed_vector_contract_and_special_members() {
        fixed_state source_state{12};
        fixed_allocator source_allocator{source_state};
        int_vector<fixed_allocator> source{source_allocator};
        source.reserve(source_state.capacity());
        require(source.capacity() >= source_state.capacity(), "full-capacity reserve did not provision vector");
        fill_reserved_vector(source, source_state.capacity());

        bool growth_failed = false;
        try {
            source.push_back(999);
        } catch (const std::length_error&) {
            growth_failed = true;
        } catch (const std::bad_alloc&) {
            growth_failed = true;
        }
        require(growth_failed, "growth beyond exclusive max must fail");
        require(source.size() == source_state.capacity(), "failed growth changed vector size");
        require(source[3] == 21, "failed growth changed vector contents");

        require_throws<std::bad_alloc>(
                [&] { int_vector<fixed_allocator> copy{source}; },
                "ordinary copy construction must fail against occupied resource");

        fixed_state copy_state{12};
        fixed_allocator copy_allocator{copy_state};
        int_vector<fixed_allocator> copy{source, copy_allocator};
        require(copy == source, "allocator-extended copy into distinct state changed values");

        source[3] = 4242;
        copy = source;
        require(copy[3] == 4242, "in-place copy assignment failed");
        require(copy.get_allocator() == copy_allocator, "copy assignment propagated exclusive allocator");

        fixed_state small_state{4};
        fixed_allocator small_allocator{small_state};
        int_vector<fixed_allocator> small{small_allocator};
        small.reserve(4);
        small.push_back(17);
        small.push_back(19);
        bool replacement_failed = false;
        try {
            small = source;
        } catch (const std::length_error&) {
            replacement_failed = true;
        } catch (const std::bad_alloc&) {
            replacement_failed = true;
        }
        require(replacement_failed, "copy assignment replacement must fail");
        require(source.size() == source_state.capacity() && source[3] == 4242,
                "failed replacement copy corrupted source");
        require(small.size() <= small.capacity(), "failed replacement copy left destination invalid");
        small.clear();
        small.push_back(23);
        require(small.size() == 1 && small.front() == 23, "destination was not reusable after failed replacement copy");

        int* source_storage = source.data();
        int_vector<fixed_allocator> moved{std::move(source)};
        require(moved.data() == source_storage, "move construction did not transfer exclusive storage");
        require(source.empty(), "moved-from exclusive vector is not empty");

        copy = std::move(moved);
        require(copy.data() == source_storage, "move assignment did not transfer exclusive storage");
        require(copy.get_allocator() == source_allocator, "move assignment did not propagate exclusive allocator");
        require(!copy_state.claimed(), "move assignment did not release destination resource");
        require(source_state.claimed(), "move assignment lost source resource claim");
    }

    void test_vector_swap_and_node_container_boundary() {
        fixed_state left_state{4};
        fixed_state right_state{4};
        fixed_allocator left_allocator{left_state};
        fixed_allocator right_allocator{right_state};
        int_vector<fixed_allocator> left{left_allocator};
        int_vector<fixed_allocator> right{right_allocator};
        left.reserve(left_state.capacity());
        right.reserve(right_state.capacity());
        left.push_back(1);
        right.push_back(2);
        int* left_storage = left.data();
        int* right_storage = right.data();
        left.swap(right);
        require(left.data() == right_storage && right.data() == left_storage,
                "vector swap did not keep allocators with storage");
        require(left.get_allocator() == right_allocator && right.get_allocator() == left_allocator,
                "vector swap did not propagate allocator identity");
        require(left.front() == 2 && right.front() == 1, "vector swap changed values");

        fixed_state node_state{64};
        fixed_allocator node_allocator{node_state};
        std::list<int, fixed_allocator> nodes{node_allocator};
        nodes.push_back(10);
        require_throws<std::bad_alloc>(
                [&] { nodes.push_back(20); }, "node container must fail on its second simultaneous node");
        require(nodes.size() == 1 && nodes.front() == 10, "failed second node corrupted first node");
    }

    void test_noheap_vector_and_sequential_thread_handoff() {
        alignas(std::max_align_t) std::array<std::byte, 16 * sizeof(int)> backing{};
        noheap_state state{backing};
        noheap_allocator allocator{state};
        {
            int_vector<noheap_allocator> values{allocator};
            values.reserve(state.capacity());
            fill_reserved_vector(values, state.capacity());
            require(values.front() == 0, "noheap vector front value changed");
            require(values.back() == static_cast<int>((state.capacity() - 1) * 7), "noheap vector back value changed");
        }
        require(!state.claimed(), "noheap vector destruction did not release state");

        int* published = nullptr;
        std::thread first{[&] {
            noheap_allocator local{allocator};
            published = local.allocate(state.capacity());
            published[0] = 808;
        }};
        first.join();
        require(published != nullptr && published[0] == 808, "first handoff thread did not publish payload");
        require(state.claimed(), "published noheap allocation lost its claim");

        int second_observed = 0;
        std::thread second{[&] {
            noheap_allocator local{allocator};
            second_observed = published[0];
            local.deallocate(published, state.capacity());
        }};
        second.join();
        require(second_observed == 808, "sequential handoff did not retain payload");
        require(!state.claimed(), "consumer handoff thread did not release claim");

        fixed_state fixed_handoff_state{8};
        fixed_allocator fixed_handoff_allocator{fixed_handoff_state};
        int* fixed_published = nullptr;
        std::thread fixed_producer{[&] {
            fixed_allocator local{fixed_handoff_allocator};
            fixed_published = local.allocate(fixed_handoff_state.capacity());
            fixed_published[0] = 909;
        }};
        fixed_producer.join();
        int fixed_observed = 0;
        std::thread fixed_consumer{[&] {
            fixed_allocator local{fixed_handoff_allocator};
            fixed_observed = fixed_published[0];
            local.deallocate(fixed_published, fixed_handoff_state.capacity());
        }};
        fixed_consumer.join();
        require(fixed_observed == 909, "fixed sequential handoff changed payload");
        require(!fixed_handoff_state.claimed(), "fixed consumer handoff did not release claim");
    }

#if defined(__linux__)
    void test_hugetlb_exact_mapping_and_no_fallback() {
        constexpr size_t huge_page_bytes = size_t{2} << 20;
        using huge_state = allocazam::
                allocazam_std_state<char, memory_mode::fixed, allocation_model::exclusive, huge_pages::enabled>;
        using huge_allocator = allocazam::
                allocazam_std_allocator<char, memory_mode::fixed, allocation_model::exclusive, huge_pages::enabled>;

        {
            huge_state state{huge_page_bytes};
            require(state.capacity() == huge_page_bytes, "exact hugetlb state changed element capacity");
            require(state.capacity_bytes() == huge_page_bytes, "exact hugetlb state changed usable bytes");
            require(state.mapping_bytes() == huge_page_bytes, "exact hugetlb state used more than one page");
            huge_allocator allocator{state};
            char* pointer = allocator.allocate(state.capacity());
            auto info = allocazam_test::mapping_for_address(pointer);
            require(info.has_value(), "failed to find exact exclusive hugetlb mapping");
            require(info->size_kb == huge_page_bytes / 1024, "exact exclusive hugetlb smaps size changed");
            require(info->kernel_page_kb == huge_page_bytes / 1024, "exclusive hugetlb kernel page size is not 2 MiB");
            require(info->hugetlb, "exclusive huge mapping is missing hugetlb VmFlag");
            allocator.deallocate(pointer, state.capacity());
        }

        {
            huge_state state{huge_page_bytes + 1};
            require(state.capacity_bytes() == huge_page_bytes + 1, "hugetlb rounding leaked into logical capacity");
            require(state.mapping_bytes() == 2 * huge_page_bytes, "just-over-page state did not map two pages");
            huge_allocator allocator{state};
            char* pointer = allocator.allocate(state.capacity());
            auto info = allocazam_test::mapping_for_address(pointer);
            require(info.has_value(), "failed to find rounded exclusive hugetlb mapping");
            require(info->size_kb == (2 * huge_page_bytes) / 1024, "rounded exclusive hugetlb smaps size is not 4 MiB");
            require(info->kernel_page_kb == huge_page_bytes / 1024, "rounded exclusive hugetlb page size is not 2 MiB");
            require(info->hugetlb, "rounded exclusive mapping is missing hugetlb VmFlag");
            allocator.deallocate(pointer, state.capacity());
        }

        struct alignas(2 * huge_page_bytes) unsupported_alignment {
            std::byte value;
        };
        using unsupported_state = allocazam::allocazam_std_state<
                unsupported_alignment,
                memory_mode::fixed,
                allocation_model::exclusive,
                huge_pages::enabled>;
        require_throws<std::bad_alloc>(
                [] { unsupported_state state{1}; }, "explicit hugetlb policy fell back for unsupported alignment");
    }
#endif
}  // namespace

int main() {
    try {
        test_fixed_state_and_exact_claim();
        test_allocate_at_least_interval_and_expand();
        test_noheap_alignment_truncation_and_reuse();
        test_rebind_identity_and_alignment_rejection();
        test_object_lifetime_alignment_and_allocator_handles();
        test_fixed_vector_contract_and_special_members();
        test_vector_swap_and_node_container_boundary();
        test_noheap_vector_and_sequential_thread_handoff();
#if defined(__linux__)
        test_hugetlb_exact_mapping_and_no_fallback();
#endif
    } catch (const std::exception& e) {
        std::cerr << "exclusive allocator smoke failed: " << e.what() << '\n';
        return 1;
    }

    std::cout << "exclusive allocator smoke: all tests passed\n";
    return 0;
}
