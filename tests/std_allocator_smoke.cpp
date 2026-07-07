#include "allocazam.hpp"
#include "utils.hpp"

#include <algorithm>
#include <barrier>
#include <deque>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <thread>
#include <unordered_map>

#if !defined(NDEBUG) && defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

namespace {
    template <size_t Words>
    struct blob_object {
        std::array<uint64_t, Words> words{};

        constexpr blob_object() = default;
        constexpr explicit blob_object(size_t seed) {
            std::ranges::for_each(std::views::iota(size_t{0}, Words), [&](size_t i) {
                words[i] = static_cast<uint64_t>((seed + 1) * (i + 3));
            });
        }

        [[nodiscard]] constexpr size_t key() const noexcept { return static_cast<size_t>(words[0]); }
    };

    using obj16 = blob_object<2>;
    using obj24 = blob_object<3>;
    using obj64 = blob_object<8>;
    using obj256 = blob_object<32>;
    using obj4k = blob_object<512>;

    struct smoke_row {
        std::string mode;
        std::string test_name;
        std::string type_name;
        size_t workload{};
        bool pass{};
        std::string note;
    };

    template <typename T>
    constexpr std::string_view type_name_of() noexcept {
        return "unknown";
    }

    template <>
    constexpr std::string_view type_name_of<int>() noexcept {
        return "int";
    }

    template <>
    constexpr std::string_view type_name_of<char>() noexcept {
        return "char";
    }

    template <>
    constexpr std::string_view type_name_of<obj16>() noexcept {
        return "obj16";
    }

    template <>
    constexpr std::string_view type_name_of<obj24>() noexcept {
        return "obj24";
    }

    template <>
    constexpr std::string_view type_name_of<obj64>() noexcept {
        return "obj64";
    }

    template <>
    constexpr std::string_view type_name_of<obj4k>() noexcept {
        return "obj4k";
    }

    template <>
    constexpr std::string_view type_name_of<obj256>() noexcept {
        return "obj256";
    }

    template <typename T>
    constexpr T make_value(size_t seed) {
        return T{seed};
    }

    template <>
    constexpr int make_value<int>(size_t seed) {
        return static_cast<int>(seed + 1);
    }

    template <>
    constexpr char make_value<char>(size_t seed) {
        return static_cast<char>('a' + (seed % 26));
    }

    template <typename T>
    constexpr size_t value_key(const T& value) {
        return value.key();
    }

    template <>
    constexpr size_t value_key<int>(const int& value) {
        return static_cast<size_t>(value);
    }

    template <>
    constexpr size_t value_key<char>(const char& value) {
        return static_cast<size_t>(value);
    }

    std::string sanitize_cell(std::string text) {
        std::ranges::for_each(text, [](char& c) {
            if (c == '\n' || c == '\r' || c == '|') {
                c = ' ';
            }
        });
        return text;
    }

    template <typename Fn>
    void add_case(
            std::vector<smoke_row>& rows,
            std::string_view mode,
            std::string_view test_name,
            std::string_view type_name,
            size_t workload,
            Fn&& fn) {
        try {
            std::string note = fn();
            if (note.empty()) {
                note = "ok";
            }

            rows.push_back(
                    smoke_row{
                            std::string{mode},
                            std::string{test_name},
                            std::string{type_name},
                            workload,
                            true,
                            sanitize_cell(std::move(note)),
                    });
        } catch (const std::exception& e) {
            rows.push_back(
                    smoke_row{
                            std::string{mode},
                            std::string{test_name},
                            std::string{type_name},
                            workload,
                            false,
                            sanitize_cell(std::string{e.what()}),
                    });
        } catch (...) {
            rows.push_back(
                    smoke_row{
                            std::string{mode},
                            std::string{test_name},
                            std::string{type_name},
                            workload,
                            false,
                            "unknown exception",
                    });
        }
    }

    template <allocazam::memory_mode Mode, typename T, typename Fn>
    void with_allocator(size_t budget, Fn&& fn) {
        if constexpr (Mode == allocazam::memory_mode::dynamic) {
            using alloc_t = allocazam::allocazam_std_allocator<T, allocazam::memory_mode::dynamic>;
            alloc_t alloc{};
            fn(alloc);
        } else if constexpr (Mode == allocazam::memory_mode::fixed) {
            using state_t = allocazam::allocazam_std_state<T, allocazam::memory_mode::fixed>;
            using alloc_t = allocazam::allocazam_std_allocator<T, allocazam::memory_mode::fixed>;
            state_t state{budget};
            alloc_t alloc{state};
            fn(alloc);
        } else {
            using state_t = allocazam::allocazam_std_state<T, allocazam::memory_mode::noheap>;
            using alloc_t = allocazam::allocazam_std_allocator<T, allocazam::memory_mode::noheap>;
            std::vector<std::byte> backing(budget);
            state_t state{std::span<std::byte>{backing.data(), backing.size()}};
            alloc_t alloc{state};
            fn(alloc);
        }
    }

    template <typename T, typename Alloc>
    std::string exercise_vector(size_t count, const Alloc& alloc) {
        std::vector<T, Alloc> values{alloc};
        values.reserve(count / 2 + 1);

        std::ranges::for_each(
                std::views::iota(size_t{0}, count), [&](size_t i) { values.push_back(make_value<T>(i)); });

        require(values.size() == count, "vector size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "vector front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "vector back mismatch");

        return "size=" + std::to_string(values.size()) + " cap=" + std::to_string(values.capacity());
    }

    template <typename T, typename Alloc>
    std::string exercise_deque(size_t count, const Alloc& alloc) {
        std::deque<T, Alloc> values{alloc};

        std::ranges::for_each(
                std::views::iota(size_t{0}, count), [&](size_t i) { values.push_back(make_value<T>(i)); });

        require(values.size() == count, "deque size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "deque front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "deque back mismatch");

        size_t pop_count = count / 3;
        std::ranges::for_each(std::views::iota(size_t{0}, pop_count), [&](size_t) { values.pop_front(); });
        require(values.size() == (count - pop_count), "deque pop size mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename T, typename Alloc>
    std::string exercise_list(size_t count, const Alloc& alloc) {
        std::list<T, Alloc> values{alloc};

        std::ranges::for_each(
                std::views::iota(size_t{0}, count), [&](size_t i) { values.push_back(make_value<T>(i)); });

        require(values.size() == count, "list size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "list front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "list back mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename Alloc>
    std::string exercise_map(size_t count, const Alloc& alloc) {
        std::map<int, int, std::less<int>, Alloc> values{std::less<int>{}, alloc};

        std::ranges::for_each(std::views::iota(size_t{0}, count), [&](size_t i) {
            auto [it, inserted] = values.emplace(static_cast<int>(i), static_cast<int>(i * 3 + 1));
            (void)it;
            require(inserted, "map emplace duplicate key");
        });

        require(values.size() == count, "map size mismatch");
        require(values.begin()->first == 0, "map first key mismatch");
        require(values.rbegin()->first == static_cast<int>(count - 1), "map last key mismatch");

        size_t stride = std::ranges::max(size_t{1}, count / 16);
        for (size_t i : std::views::iota(size_t{0}, count)) {
            if ((i % stride) != 0) {
                continue;
            }

            auto it = values.find(static_cast<int>(i));
            require(it != values.end(), "map find failed");
            require(it->second == static_cast<int>(i * 3 + 1), "map value mismatch");
        }

        size_t erase_count = count / 4;
        std::ranges::for_each(
                std::views::iota(size_t{0}, erase_count), [&](size_t i) { values.erase(static_cast<int>(i)); });
        require(values.size() == (count - erase_count), "map erase size mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename Alloc>
    std::string exercise_unordered_map(size_t count, const Alloc& alloc) {
        std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, Alloc> values{
                0, std::hash<int>{}, std::equal_to<int>{}, alloc};
        values.reserve(count);

        std::ranges::for_each(std::views::iota(size_t{0}, count), [&](size_t i) {
            auto [it, inserted] = values.emplace(static_cast<int>(i), static_cast<int>(i * 5 + 7));
            (void)it;
            require(inserted, "unordered_map emplace duplicate key");
        });

        require(values.size() == count, "unordered_map size mismatch");

        size_t stride = std::ranges::max(size_t{1}, count / 16);
        for (size_t i : std::views::iota(size_t{0}, count)) {
            if ((i % stride) != 0) {
                continue;
            }

            auto it = values.find(static_cast<int>(i));
            require(it != values.end(), "unordered_map find failed");
            require(it->second == static_cast<int>(i * 5 + 7), "unordered_map value mismatch");
        }

        size_t erase_count = count / 4;
        std::ranges::for_each(
                std::views::iota(size_t{0}, erase_count), [&](size_t i) { values.erase(static_cast<int>(i)); });
        require(values.size() == (count - erase_count), "unordered_map erase size mismatch");

        return "size=" + std::to_string(values.size()) + " buckets=" + std::to_string(values.bucket_count());
    }

    template <typename Alloc>
    std::string exercise_string(size_t length, const Alloc& alloc) {
        std::basic_string<char, std::char_traits<char>, Alloc> s{alloc};
        s.reserve(length);

        std::ranges::for_each(
                std::views::iota(size_t{0}, length), [&](size_t i) { s.push_back(static_cast<char>('a' + (i % 26))); });

        require(s.size() == length, "string size mismatch");
        require(s.front() == 'a', "string front mismatch");
        require(s.back() == static_cast<char>('a' + ((length - 1) % 26)), "string back mismatch");

        return "size=" + std::to_string(s.size()) + " cap=" + std::to_string(s.capacity());
    }

    template <typename T, typename Alloc>
    std::string exercise_round_trip(size_t n, const Alloc& alloc) {
        Alloc a = alloc;
        T* p = a.allocate(n);
        require(p != nullptr, "allocate(n) returned null");

        std::ranges::for_each(std::views::iota(size_t{0}, n), [&](size_t i) { a.construct(p + i, make_value<T>(i)); });

        std::ranges::for_each(std::views::iota(size_t{0}, n), [&](size_t i) {
            require(value_key(*(p + i)) == value_key(make_value<T>(i)), "allocate/construct round-trip mismatch");
            a.destroy(p + i);
        });

        a.deallocate(p, n);
        return "n=" + std::to_string(n);
    }

    std::string exercise_std_allocator_overflow_count_guard() {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        alloc_t alloc{};

        bool saw_bad_length = false;
        try {
            (void)alloc.allocate(alloc.max_size() + 1);
        } catch (const std::bad_array_new_length&) {
            saw_bad_length = true;
        }

        require(saw_bad_length, "std allocator overflow count guard should throw bad_array_new_length");
        return "bad_array_new_length";
    }

    std::string exercise_chunk_overflow_guard() {
        using node_t = allocazam::node_t<obj16>;
        size_t overflow_count = (std::numeric_limits<size_t>::max() / sizeof(node_t)) + 1;

        bool saw_bad_length = false;
        try {
            allocazam::chunk_t<obj16, true> chunk{overflow_count, 4096, alignof(node_t)};
            (void)chunk;
        } catch (const std::bad_array_new_length&) {
            saw_bad_length = true;
        }

        require(saw_bad_length, "chunk overflow guard should throw bad_array_new_length");
        return "bad_array_new_length";
    }

    std::string exercise_runner_overflow_guard() {
        using runner_t = allocazam::runner::allocator<true>;
        runner_t runs{65536};

        void* immediate_fail = runs.allocate_bytes(std::numeric_limits<size_t>::max(), alignof(std::max_align_t));
        require(immediate_fail == nullptr, "runner overflow add should fail with null");

        bool saw_bad_alloc = false;
        try {
            (void)runs.allocate_bytes(std::numeric_limits<size_t>::max() - 64, alignof(std::max_align_t));
        } catch (const std::bad_alloc&) {
            saw_bad_alloc = true;
        }
        require(saw_bad_alloc, "runner overflow grow should throw bad_alloc");

        bool saw_ctor_bad_alloc = false;
        try {
            runner_t huge{std::numeric_limits<size_t>::max()};
            (void)huge;
        } catch (const std::bad_alloc&) {
            saw_ctor_bad_alloc = true;
        }
        require(saw_ctor_bad_alloc, "runner constructor overflow should throw bad_alloc");

        return "safe-fail";
    }

    template <allocazam::memory_mode Mode>
    std::string exercise_map_rebind_explicit_state(
            size_t state_budget, size_t max_inserts, size_t max_allowed_inserts) {
        using pair_t = std::pair<const int, int>;
        using state_t = allocazam::allocazam_std_state<pair_t, Mode>;
        using alloc_t = allocazam::allocazam_std_allocator<pair_t, Mode>;
        using map_t = std::map<int, int, std::less<int>, alloc_t>;

        size_t inserted = 0;
        bool saw_bad_alloc = false;

        auto run = [&](const alloc_t& alloc) {
            map_t values{std::less<int>{}, alloc};
            try {
                for (; inserted < max_inserts; ++inserted) {
                    values.emplace(static_cast<int>(inserted), static_cast<int>(inserted * 7 + 3));
                }
            } catch (const std::bad_alloc&) {
                saw_bad_alloc = true;
            }

            require(values.size() == inserted, "map explicit-state size mismatch");
            if (inserted != 0) {
                require(values.begin()->first == 0, "map explicit-state first key mismatch");
                require(values.find(static_cast<int>(inserted - 1)) != values.end(),
                        "map explicit-state last key missing");
            }
        };

        if constexpr (Mode == allocazam::memory_mode::fixed) {
            state_t state{state_budget};
            alloc_t alloc{state};
            run(alloc);
        } else if constexpr (Mode == allocazam::memory_mode::noheap) {
            std::vector<std::byte> backing(state_budget);
            state_t state{std::span<std::byte>{backing.data(), backing.size()}};
            alloc_t alloc{state};
            run(alloc);
        } else {
            static_assert(Mode != allocazam::memory_mode::dynamic, "explicit-state rebind test supports fixed/noheap");
        }

        require(saw_bad_alloc, "map explicit-state rebind should hit bad_alloc under tiny bounded state");
        require(inserted <= max_allowed_inserts, "map explicit-state rebind inserted unexpectedly high element count");
        return "inserted=" + std::to_string(inserted);
    }

    template <allocazam::memory_mode Mode>
    std::string exercise_unordered_map_rebind_explicit_state(
            size_t state_budget, size_t max_inserts, size_t max_allowed_inserts) {
        using pair_t = std::pair<const int, int>;
        using state_t = allocazam::allocazam_std_state<pair_t, Mode>;
        using alloc_t = allocazam::allocazam_std_allocator<pair_t, Mode>;
        using unordered_map_t = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, alloc_t>;

        size_t inserted = 0;
        bool saw_bad_alloc = false;

        auto run = [&](const alloc_t& alloc) {
            unordered_map_t values{0, std::hash<int>{}, std::equal_to<int>{}, alloc};
            try {
                for (; inserted < max_inserts; ++inserted) {
                    values.emplace(static_cast<int>(inserted), static_cast<int>(inserted * 11 + 1));
                }
            } catch (const std::bad_alloc&) {
                saw_bad_alloc = true;
            }

            require(values.size() == inserted, "unordered_map explicit-state size mismatch");
            if (inserted != 0) {
                require(values.find(0) != values.end(), "unordered_map explicit-state key 0 missing");
                require(values.find(static_cast<int>(inserted - 1)) != values.end(),
                        "unordered_map explicit-state last key missing");
            }
        };

        if constexpr (Mode == allocazam::memory_mode::fixed) {
            state_t state{state_budget};
            alloc_t alloc{state};
            run(alloc);
        } else if constexpr (Mode == allocazam::memory_mode::noheap) {
            std::vector<std::byte> backing(state_budget);
            state_t state{std::span<std::byte>{backing.data(), backing.size()}};
            alloc_t alloc{state};
            run(alloc);
        } else {
            static_assert(Mode != allocazam::memory_mode::dynamic, "explicit-state rebind test supports fixed/noheap");
        }

        require(saw_bad_alloc, "unordered_map explicit-state rebind should hit bad_alloc under tiny bounded state");
        require(inserted <= max_allowed_inserts,
                "unordered_map explicit-state rebind inserted unexpectedly high element count");
        return "inserted=" + std::to_string(inserted);
    }

    template <typename T>
    std::string exercise_granule_headers(size_t initial_count, size_t alloc_count) {
        using pool_t = allocazam::allocazam<T, allocazam::memory_mode::dynamic, true>;
        pool_t pool{initial_count};

        size_t granule = pool_t::granule_bytes(allocazam::detail::detect_page_size());
        require(std::has_single_bit(granule), "granule must be a power of 2");

        std::vector<T*> ptrs;
        ptrs.reserve(alloc_count);
        std::ranges::for_each(std::views::iota(size_t{0}, alloc_count), [&](size_t) {
            T* p = pool.allocate();
            require(p != nullptr, "headered pool allocate returned null");

            auto* header =
                    reinterpret_cast<const allocazam::granule_header*>(reinterpret_cast<uintptr_t>(p) & ~(granule - 1));
            require(header->owner == &pool, "granule mask must resolve the owning pool");
            require(header->debug_tag == allocazam::granule_debug_tag, "granule debug tag mismatch");
            ptrs.push_back(p);
        });

        require(pool.capacity() > initial_count, "exercise must span chunk growth");
        std::ranges::for_each(ptrs, [&](T* p) { pool.deallocate(p); });
        require(pool.size() == 0, "all nodes must return to the pool");

        return "granule=" + std::to_string(granule) + " cap=" + std::to_string(pool.capacity());
    }

    template <allocazam::memory_mode Mode>
    std::string exercise_pool_remote_drain(size_t initial_count) {
        using pool_t = allocazam::allocazam<int, Mode, true>;
        pool_t pool{initial_count};
        size_t cap = pool.capacity();
        require(cap == initial_count, "requested capacity must be exact, not granule-rounded");

        std::vector<int*> ptrs;
        ptrs.reserve(cap);
        std::ranges::for_each(std::views::iota(size_t{0}, cap), [&](size_t) {
            int* p = pool.allocate();
            require(p != nullptr, "headered pool allocate returned null before exhaustion");
            ptrs.push_back(p);
        });

        if constexpr (Mode == allocazam::memory_mode::fixed) {
            require(pool.allocate() == nullptr, "exhausted fixed pool must return nullptr");
        }

        std::ranges::for_each(ptrs, [&](int* p) { pool.remote_push(p); });
        require(pool.size() == cap, "remotely parked nodes must count as live until drain");

        int* p = pool.allocate();
        require(p != nullptr, "drain must satisfy allocation with all nodes parked remotely");
        require(pool.capacity() == cap, "drain must satisfy allocation without growth");
        require(pool.size() == 1, "drain must reconcile size accounting");
        pool.deallocate(p);

        return "cap=" + std::to_string(cap);
    }

    // test case 2 + 3: alloc on A to exact capacity, free on B (routes remotely), A
    // allocates again — the drain must satisfy it before growth, with every node
    // accounted. oracle: state() on the worker plus registry introspection
    template <allocazam::memory_mode Mode>
    std::string exercise_cross_thread_free_drain() {
        using alloc_t = allocazam::allocazam_std_allocator<int, Mode>;
        using state_t = typename alloc_t::state_type;

        std::barrier<> sync{2};
        state_t* worker_state = nullptr;
        std::vector<int*> ptrs;
        size_t cap = 0;
        std::string worker_error;

        std::thread worker{[&] {
            try {
                alloc_t alloc{};
                state_t* s = alloc.state();
                worker_state = s;

                ptrs.push_back(alloc.allocate(1));
                cap = s->pool.capacity();
                while (ptrs.size() < cap) {
                    ptrs.push_back(alloc.allocate(1));
                }
                require(s->pool.size() == cap, "worker must hold every node before handoff");
            } catch (const std::exception& e) {
                worker_error = e.what();
            }
            sync.arrive_and_wait();
            sync.arrive_and_wait();
            if (!worker_error.empty()) {
                return;
            }
            try {
                alloc_t alloc{};
                require(worker_state->pool.size() == cap, "remotely freed nodes must count live until drain");
                int* p = alloc.allocate(1);
                require(p != nullptr, "allocation after remote frees must succeed");
                require(worker_state->pool.capacity() == cap, "drain must satisfy allocation before growth");
                require(worker_state->pool.size() == 1, "every remotely freed node must be accounted for");
                alloc.deallocate(p, 1);
            } catch (const std::exception& e) {
                worker_error = e.what();
            }
        }};

        sync.arrive_and_wait();
        if (worker_error.empty()) {
            alloc_t alloc{};
            std::ranges::for_each(ptrs, [&](int* p) { alloc.deallocate(p, 1); });
        }
        sync.arrive_and_wait();
        worker.join();
        require(worker_error.empty(), worker_error.c_str());

        bool found = false;
        for (state_t* s = alloc_t::thread_state_registry(); s != nullptr; s = s->registry_next) {
            found = found || (s == worker_state);
        }
        require(found, "worker state must be registry-reachable");

        return "cap=" + std::to_string(cap);
    }

    // test case 4: thread exits with outstanding allocations; remote frees land after
    // exit with no use-after-free — the registry holds the orphaned state
    std::string exercise_orphaned_state_safety(size_t count) {
        using alloc_t = allocazam::allocazam_std_allocator<obj16, allocazam::memory_mode::dynamic>;
        using state_t = alloc_t::state_type;

        std::vector<obj16*> ptrs(count, nullptr);
        state_t* worker_state = nullptr;

        std::thread worker{[&] {
            alloc_t alloc{};
            std::ranges::for_each(ptrs, [&](obj16*& p) { p = alloc.allocate(1); });
            worker_state = alloc.state();
        }};
        worker.join();

        require(worker_state != nullptr, "worker state must exist");
        require(worker_state->pool.size() == count, "orphaned state must hold the outstanding nodes");

        bool found = false;
        for (state_t* s = alloc_t::thread_state_registry(); s != nullptr; s = s->registry_next) {
            found = found || (s == worker_state);
        }
        require(found, "orphaned state must stay registry-reachable");

        alloc_t alloc{};
        std::ranges::for_each(ptrs, [&](obj16* p) { alloc.deallocate(p, 1); });
        require(worker_state->pool.size() == count, "remotely parked nodes count live until the owner drains");

        return "count=" + std::to_string(count);
    }

    // test case 7: state() thread-locality, stability, rebind resolution, first touch
    std::string exercise_state_semantics() {
        using alloc_t = allocazam::allocazam_std_allocator<obj64, allocazam::memory_mode::dynamic>;
        using state_t = alloc_t::state_type;
        using rebound_t = alloc_t::rebind<obj256>::other;

        alloc_t a{};
        state_t* s1 = a.state();
        require(s1 != nullptr, "state() must create the thread's state on first touch");
        require(a.state() == s1, "state() must be stable within a thread");

        alloc_t b{};
        require(b.state() == s1, "same thread and instantiation must resolve the same state");

        state_t* worker_seen = nullptr;
        std::thread worker{[&] {
            alloc_t c{};
            worker_seen = c.state();
        }};
        worker.join();
        require(worker_seen != nullptr, "worker state() must create");
        require(worker_seen != s1, "distinct threads must see distinct states");

        rebound_t rebound_stateless{a};
        auto* ru = rebound_stateless.state();
        require(ru != nullptr, "rebound-from-stateless must resolve the thread's U state");
        rebound_t direct{};
        require(direct.state() == ru, "rebound resolution must equal direct default resolution");

        state_t bound_state{64, 65536};
        alloc_t bound{bound_state};
        require(bound.state() == &bound_state, "explicit binding must return the bound state");
        rebound_t rebound_explicit{bound};
        require(rebound_explicit.state() == nullptr, "rebound-from-explicit holds no state_type");

        return "ok";
    }

    // allocator equality is the deallocation-interchange contract: post-flip, two
    // stateless allocators are {null, null} and interchange on any thread via owner
    // routing; explicit and runs-override bindings interchange only with their own kind
    std::string exercise_allocator_equality() {
        using alloc_t = allocazam::allocazam_std_allocator<obj64, allocazam::memory_mode::dynamic>;
        using state_t = alloc_t::state_type;
        using rebound_t = alloc_t::rebind<obj256>::other;

        alloc_t a{};
        alloc_t b{};
        require(a == b, "stateless allocators must compare equal");

        state_t s1{64, 65536};
        state_t s2{64, 65536};
        alloc_t e1{s1};
        alloc_t e1_alias{s1};
        alloc_t e2{s2};
        require(e1 == e1_alias, "allocators bound to the same state must compare equal");
        require(e1 != e2, "allocators bound to distinct states must compare unequal");
        require(a != e1, "stateless and explicit-state allocators must compare unequal");

        rebound_t r1{e1};
        rebound_t r1_alias{e1};
        rebound_t r2{e2};
        require(r1 == r1_alias, "rebinds sharing a runs override must compare equal");
        require(r1 != r2, "rebinds over distinct runs overrides must compare unequal");

        rebound_t rs{a};
        rebound_t rs_direct{};
        require(rs == rs_direct, "rebound-from-stateless must stay stateless and compare equal");
        require(rs != r1, "stateless and runs-override rebinds must compare unequal");

        return "ok";
    }

    // test case 8: routed frees into an explicit pool recycle at the owner's next
    // empty-list event, and pending parked frees reconcile in the destructor drain
    std::string exercise_explicit_pool_dtor_drain(size_t count) {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using state_t = alloc_t::state_type;

        auto free_remotely = [](std::vector<int*>& ptrs) {
            std::thread t{[&] {
                alloc_t stateless{};
                std::ranges::for_each(ptrs, [&](int* p) { stateless.deallocate(p, 1); });
            }};
            t.join();
        };

        // recycle: owner exhausts the free list, then the drain feeds it
        {
            state_t state{1024, 65536};
            alloc_t bound{state};

            std::vector<int*> ptrs(count, nullptr);
            std::ranges::for_each(ptrs, [&](int*& p) { p = bound.allocate(1); });
            free_remotely(ptrs);
            require(state.pool.size() == count, "routed frees must park until the owner drains");

            size_t cap = state.pool.capacity();
            std::vector<int*> refill;
            refill.reserve(cap - count + 1);
            std::ranges::for_each(std::views::iota(size_t{0}, cap - count + 1), [&](size_t) {
                int* p = bound.allocate(1);
                require(p != nullptr, "recycle allocation failed");
                refill.push_back(p);
            });
            require(state.pool.capacity() == cap, "parked nodes must recycle before growth");
            std::ranges::for_each(refill, [&](int* p) { bound.deallocate(p, 1); });
            require(state.pool.size() == 0, "accounting must be exact after drain");
        }

        // destructor drain: parked frees still pending at destruction reconcile without
        // a spurious outstanding-objects assert
        {
            state_t state{1024, 65536};
            alloc_t bound{state};

            std::vector<int*> ptrs(count, nullptr);
            std::ranges::for_each(ptrs, [&](int*& p) { p = bound.allocate(1); });
            free_remotely(ptrs);
            require(state.pool.size() == count, "routed frees must park until destruction");
        }

        return "count=" + std::to_string(count);
    }

#if !defined(NDEBUG) && defined(__linux__)
    // test case 5: the canary asserts stay exact on the local path, and the granule
    // debug tag catches stateless frees of raw pool memory. fork-based: the child must
    // die on SIGABRT. all sibling test threads are joined by the time this runs
    template <typename Fn>
    [[nodiscard]] bool expect_abort(Fn&& fn) {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::close(2);
            fn();
            ::_exit(0);
        }
        if (pid < 0) {
            return false;
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    }

    std::string exercise_debug_assert_semantics() {
        bool double_free_aborts = expect_abort([] {
            using state_t = allocazam::allocazam_std_state<int, allocazam::memory_mode::dynamic>;
            using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
            state_t state{64, 65536};
            alloc_t alloc{state};
            int* p = alloc.allocate(1);
            alloc.deallocate(p, 1);
            alloc.deallocate(p, 1);
        });
        require(double_free_aborts, "double free through an explicit pool must trip the canary");

        bool raw_free_aborts = expect_abort([] {
            allocazam::allocazam<int, allocazam::memory_mode::dynamic> raw_pool{64};
            int* p = raw_pool.allocate();
            allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic> stateless{};
            stateless.deallocate(p, 1);
        });
        require(raw_free_aborts, "stateless free of raw pool memory must trip the granule tag assert");

        // remote-path validation lives at drain time: a locally-freed node parked again
        // on the remote stack must trip the drain's free-list canary (here via the
        // destructor drain)
        bool pool_remote_double_free_aborts = expect_abort([] {
            allocazam::allocazam<int, allocazam::memory_mode::dynamic, true> pool{64};
            int* p = pool.allocate();
            pool.deallocate(p);
            pool.remote_push(p);
        });
        require(pool_remote_double_free_aborts, "remote double free must trip the pool drain canary");

        // runner variant: a freed run pushed remotely still carries its free flag when
        // the find-fit-failure drain replays it
        bool runner_remote_double_free_aborts = expect_abort([] {
            allocazam::runner::allocator<true> runs{65536};
            void* anchor = runs.allocate_bytes(64, 16);
            void* p = runs.allocate_bytes(256, 16);
            runs.deallocate_bytes(p);
            runs.remote_push(p);
            (void)runs.allocate_bytes(size_t{1} << 20, 16);  // find-fit failure forces the drain
            (void)anchor;
        });
        require(runner_remote_double_free_aborts, "remote double free must trip the runner drain canary");

        return "ok";
    }
#else
    std::string exercise_debug_assert_semantics() {
        return "skipped (NDEBUG or non-linux)";
    }
#endif

    std::string exercise_tls_cache_thread_exit_reclaim(size_t thread_count, size_t request_count) {
        using alloc_t = allocazam::allocazam_std_allocator<char, allocazam::memory_mode::fixed>;
        size_t succeeded = 0;

        std::ranges::for_each(std::views::iota(size_t{0}, thread_count), [&](size_t) {
            bool ok = false;
            std::thread worker{[&] {
                try {
                    alloc_t alloc{};
                    std::ranges::for_each(std::views::iota(size_t{0}, request_count), [&](size_t) {
                        char* p = alloc.allocate(64);
                        require(p != nullptr, "tls cache thread worker allocate returned null");
                        alloc.deallocate(p, 64);
                    });
                    ok = true;
                } catch (...) {
                    ok = false;
                }
            }};
            worker.join();
            require(ok, "tls cache thread-exit reclaim failed under thread churn");
            ++succeeded;
        });

        return "threads=" + std::to_string(succeeded);
    }

    template <allocazam::memory_mode Mode, typename T, typename Fn>
    void add_mode_case(
            std::vector<smoke_row>& rows, std::string_view test_name, size_t workload, size_t budget, Fn&& fn) {
        add_case(rows, memory_mode_to_string(Mode), test_name, type_name_of<T>(), workload, [&]() -> std::string {
            std::string note;
            with_allocator<Mode, T>(budget, [&](const auto& alloc) { note = fn(alloc); });
            return note;
        });
    }

    template <allocazam::memory_mode Mode>
    void add_mode_matrix(std::vector<smoke_row>& rows) {
        size_t base_budget = 0;
        size_t char_budget = 0;
        size_t int_count = 0;
        size_t list_count = 0;
        size_t deque_count = 0;
        size_t obj16_count = 0;
        size_t obj64_count = 0;
        size_t obj256_count = 0;
        size_t map_count = 0;
        size_t unordered_map_count = 0;

        std::array<size_t, 4> lengths{};

        if constexpr (Mode == allocazam::memory_mode::dynamic) {
            base_budget = 0;
            char_budget = 0;
            int_count = 4096;
            list_count = 2048;
            deque_count = 4096;
            obj16_count = 2048;
            obj64_count = 1024;
            obj256_count = 256;
            map_count = 2048;
            unordered_map_count = 2048;
            lengths = {31, 127, 511, 2047};
        } else if constexpr (Mode == allocazam::memory_mode::fixed) {
            base_budget = 16384;
            char_budget = 32768;
            int_count = 2048;
            list_count = 1024;
            deque_count = 2048;
            obj16_count = 1024;
            obj64_count = 512;
            obj256_count = 128;
            map_count = 1024;
            unordered_map_count = 1024;
            lengths = {31, 127, 511, 1023};
        } else {
            base_budget = 1 << 20;
            char_budget = 1 << 20;
            int_count = 1024;
            list_count = 512;
            deque_count = 1024;
            obj16_count = 512;
            obj64_count = 256;
            obj256_count = 64;
            map_count = 512;
            unordered_map_count = 512;
            lengths = {31, 127, 511, 1023};
        }

        if constexpr (Mode == allocazam::memory_mode::noheap) {
            add_mode_case<Mode, int>(rows, "allocator_round_trip", 64, base_budget, [&](const auto& alloc) {
                return exercise_round_trip<int>(64, alloc);
            });
            add_mode_case<Mode, obj16>(rows, "allocator_round_trip", 32, base_budget, [&](const auto& alloc) {
                return exercise_round_trip<obj16>(32, alloc);
            });
            add_mode_case<Mode, obj64>(rows, "allocator_round_trip", 16, base_budget, [&](const auto& alloc) {
                return exercise_round_trip<obj64>(16, alloc);
            });
            add_mode_case<Mode, obj256>(rows, "allocator_round_trip", 8, base_budget, [&](const auto& alloc) {
                return exercise_round_trip<obj256>(8, alloc);
            });
            add_mode_case<Mode, char>(rows, "allocator_round_trip", 512, char_budget, [&](const auto& alloc) {
                return exercise_round_trip<char>(512, alloc);
            });
            add_case(rows, memory_mode_to_string(Mode), "map_rebind_explicit_state", "pair<int,int>", 1024, [&]() {
                return exercise_map_rebind_explicit_state<Mode>(4096, 1024, 128);
            });
            add_case(
                    rows,
                    memory_mode_to_string(Mode),
                    "unordered_map_rebind_explicit_state",
                    "pair<int,int>",
                    1024,
                    [&]() { return exercise_unordered_map_rebind_explicit_state<Mode>(4096, 1024, 128); });
            add_case(rows, memory_mode_to_string(Mode), "container_matrix", "n/a", 0, []() -> std::string {
                return "skipped (noheap rebind for list/deque broad matrix not implemented)";
            });
        } else {
            add_mode_case<Mode, int>(rows, "vector_push_back", int_count, base_budget, [&](const auto& alloc) {
                return exercise_vector<int>(int_count, alloc);
            });
            add_mode_case<Mode, int>(rows, "deque_push_pop", deque_count, base_budget, [&](const auto& alloc) {
                return exercise_deque<int>(deque_count, alloc);
            });
            add_mode_case<Mode, int>(rows, "list_push_back", list_count, base_budget, [&](const auto& alloc) {
                return exercise_list<int>(list_count, alloc);
            });
            add_mode_case<Mode, obj16>(rows, "deque_push_pop", deque_count / 2, base_budget, [&](const auto& alloc) {
                return exercise_deque<obj16>(deque_count / 2, alloc);
            });
            add_mode_case<Mode, obj16>(rows, "list_push_back", list_count / 2, base_budget, [&](const auto& alloc) {
                return exercise_list<obj16>(list_count / 2, alloc);
            });

            add_mode_case<Mode, obj16>(rows, "vector_push_back", obj16_count, base_budget, [&](const auto& alloc) {
                return exercise_vector<obj16>(obj16_count, alloc);
            });
            add_mode_case<Mode, obj64>(rows, "vector_push_back", obj64_count, base_budget, [&](const auto& alloc) {
                return exercise_vector<obj64>(obj64_count, alloc);
            });
            add_mode_case<Mode, obj256>(rows, "vector_push_back", obj256_count, base_budget, [&](const auto& alloc) {
                return exercise_vector<obj256>(obj256_count, alloc);
            });

            std::ranges::for_each(lengths, [&](size_t len) {
                add_mode_case<Mode, char>(rows, "string_push_back", len, char_budget, [&](const auto& alloc) {
                    return exercise_string(len, alloc);
                });
            });

            add_mode_case<Mode, int>(rows, "allocator_round_trip", 64, base_budget, [&](const auto& alloc) {
                return exercise_round_trip<int>(64, alloc);
            });

            add_case(rows, memory_mode_to_string(Mode), "map_insert_find", "pair<int,int>", map_count, [&]() {
                std::string note;
                with_allocator<Mode, std::pair<const int, int>>(
                        base_budget, [&](const auto& alloc) { note = exercise_map(map_count, alloc); });
                return note;
            });

            add_case(
                    rows,
                    memory_mode_to_string(Mode),
                    "unordered_map_insert_find",
                    "pair<int,int>",
                    unordered_map_count,
                    [&]() {
                        std::string note;
                        with_allocator<Mode, std::pair<const int, int>>(base_budget, [&](const auto& alloc) {
                            note = exercise_unordered_map(unordered_map_count, alloc);
                        });
                        return note;
                    });

            if constexpr (Mode == allocazam::memory_mode::dynamic) {
                add_case(rows, memory_mode_to_string(Mode), "overflow_std_count_guard", "int", 0, []() {
                    return exercise_std_allocator_overflow_count_guard();
                });
                add_case(rows, memory_mode_to_string(Mode), "overflow_chunk_guard", "obj16", 0, []() {
                    return exercise_chunk_overflow_guard();
                });
                add_case(rows, memory_mode_to_string(Mode), "overflow_runner_guard", "n/a", 0, []() {
                    return exercise_runner_overflow_guard();
                });
            }

            if constexpr (Mode == allocazam::memory_mode::fixed) {
                add_case(rows, memory_mode_to_string(Mode), "map_rebind_explicit_state", "pair<int,int>", 1024, [&]() {
                    return exercise_map_rebind_explicit_state<Mode>(1, 1024, 256);
                });
                add_case(
                        rows,
                        memory_mode_to_string(Mode),
                        "unordered_map_rebind_explicit_state",
                        "pair<int,int>",
                        1024,
                        [&]() { return exercise_unordered_map_rebind_explicit_state<Mode>(1, 1024, 256); });
                add_case(rows, memory_mode_to_string(Mode), "tls_cache_thread_exit_reclaim", "char", 96, [&]() {
                    return exercise_tls_cache_thread_exit_reclaim(96, 1);
                });
            }
        }
    }

    void print_table(const std::vector<smoke_row>& rows) {
        std::cout << "\nsmoke test coverage matrix\n";
        std::cout << std::left;
        std::cout << std::setw(10) << "mode" << std::setw(28) << "test" << std::setw(16) << "type" << std::setw(10)
                  << "workload" << std::setw(8) << "result" << "note\n";
        std::cout << std::setw(10) << "--------" << std::setw(28) << "----------------------------" << std::setw(16)
                  << "--------------" << std::setw(10) << "--------" << std::setw(8) << "------"
                  << "----------------------------------------\n";

        std::ranges::for_each(rows, [](const smoke_row& row) {
            std::cout << std::setw(10) << row.mode << std::setw(28) << row.test_name << std::setw(16) << row.type_name
                      << std::setw(10) << row.workload << std::setw(8) << (row.pass ? "PASS" : "FAIL") << row.note
                      << "\n";
        });

        size_t pass_count =
                static_cast<size_t>(std::ranges::count_if(rows, [](const smoke_row& row) { return row.pass; }));

        std::cout << "\nsummary: " << pass_count << "/" << rows.size() << " cases passed\n";
    }
}  // namespace

int main() {
    std::vector<smoke_row> rows;
    rows.reserve(64);

    add_mode_matrix<allocazam::memory_mode::dynamic>(rows);
    add_mode_matrix<allocazam::memory_mode::fixed>(rows);
    add_mode_matrix<allocazam::memory_mode::noheap>(rows);

    add_case(rows, "granule", "owner_mask_resolution", "int", 600, [] {
        return exercise_granule_headers<int>(64, 600);
    });
    add_case(rows, "granule", "owner_mask_resolution", "obj24", 600, [] {
        return exercise_granule_headers<obj24>(64, 600);
    });
    add_case(rows, "granule", "owner_mask_resolution", "obj4k", 20, [] {
        return exercise_granule_headers<obj4k>(4, 20);
    });
    add_case(rows, "granule", "remote_drain_all_parked", "int", 64, [] {
        return exercise_pool_remote_drain<allocazam::memory_mode::fixed>(64);
    });
    add_case(rows, "granule", "remote_drain_no_grow", "int", 64, [] {
        return exercise_pool_remote_drain<allocazam::memory_mode::dynamic>(64);
    });

    add_case(rows, "threaded", "cross_thread_free_drain", "int", 0, [] {
        return exercise_cross_thread_free_drain<allocazam::memory_mode::dynamic>();
    });
    add_case(rows, "threaded", "cross_thread_free_drain_fixed", "int", 0, [] {
        return exercise_cross_thread_free_drain<allocazam::memory_mode::fixed>();
    });
    add_case(rows, "threaded", "orphaned_state_safety", "obj16", 512, [] {
        return exercise_orphaned_state_safety(512);
    });
    add_case(rows, "threaded", "state_semantics", "obj64", 0, [] { return exercise_state_semantics(); });
    add_case(rows, "threaded", "allocator_equality", "obj64", 0, [] { return exercise_allocator_equality(); });
    add_case(rows, "threaded", "explicit_pool_dtor_drain", "int", 256, [] {
        return exercise_explicit_pool_dtor_drain(256);
    });
    add_case(rows, "asserts", "debug_assert_semantics", "int", 0, [] { return exercise_debug_assert_semantics(); });

    print_table(rows);

    if (std::ranges::any_of(rows, [](const smoke_row& row) { return !row.pass; })) {
        return 1;
    }

    return 0;
}
