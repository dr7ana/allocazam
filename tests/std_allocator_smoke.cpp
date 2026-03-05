#include "utils.hpp"

#include <deque>
#include <iomanip>
#include <list>
#include <map>
#include <unordered_map>

namespace {
    template <size_t Words>
    struct blob_object {
        std::array<uint64_t, Words> words{};

        constexpr blob_object() = default;
        constexpr explicit blob_object(size_t seed) {
            for (size_t i : std::ranges::iota_view{size_t{0}, Words}) {
                words[i] = static_cast<uint64_t>((seed + 1) * (i + 3));
            }
        }

        [[nodiscard]] constexpr size_t key() const noexcept { return static_cast<size_t>(words[0]); }
    };

    using obj16 = blob_object<2>;
    using obj64 = blob_object<8>;
    using obj256 = blob_object<32>;

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
    constexpr std::string_view type_name_of<obj64>() noexcept {
        return "obj64";
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
        for (char& c : text) {
            if (c == '\n' || c == '\r' || c == '|') {
                c = ' ';
            }
        }
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

        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            values.push_back(make_value<T>(i));
        }

        require(values.size() == count, "vector size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "vector front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "vector back mismatch");

        return "size=" + std::to_string(values.size()) + " cap=" + std::to_string(values.capacity());
    }

    template <typename T, typename Alloc>
    std::string exercise_deque(size_t count, const Alloc& alloc) {
        std::deque<T, Alloc> values{alloc};

        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            values.push_back(make_value<T>(i));
        }

        require(values.size() == count, "deque size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "deque front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "deque back mismatch");

        size_t pop_count = count / 3;
        for (size_t i : std::ranges::iota_view{size_t{0}, pop_count}) {
            (void)i;
            values.pop_front();
        }
        require(values.size() == (count - pop_count), "deque pop size mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename T, typename Alloc>
    std::string exercise_list(size_t count, const Alloc& alloc) {
        std::list<T, Alloc> values{alloc};

        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            values.push_back(make_value<T>(i));
        }

        require(values.size() == count, "list size mismatch");
        require(value_key(values.front()) == value_key(make_value<T>(0)), "list front mismatch");
        require(value_key(values.back()) == value_key(make_value<T>(count - 1)), "list back mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename Alloc>
    std::string exercise_map(size_t count, const Alloc& alloc) {
        std::map<int, int, std::less<int>, Alloc> values{std::less<int>{}, alloc};

        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            auto [it, inserted] = values.emplace(static_cast<int>(i), static_cast<int>(i * 3 + 1));
            (void)it;
            require(inserted, "map emplace duplicate key");
        }

        require(values.size() == count, "map size mismatch");
        require(values.begin()->first == 0, "map first key mismatch");
        require(values.rbegin()->first == static_cast<int>(count - 1), "map last key mismatch");

        size_t stride = std::ranges::max(size_t{1}, count / 16);
        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            if ((i % stride) != 0) {
                continue;
            }

            auto it = values.find(static_cast<int>(i));
            require(it != values.end(), "map find failed");
            require(it->second == static_cast<int>(i * 3 + 1), "map value mismatch");
        }

        size_t erase_count = count / 4;
        for (size_t i : std::ranges::iota_view{size_t{0}, erase_count}) {
            values.erase(static_cast<int>(i));
        }
        require(values.size() == (count - erase_count), "map erase size mismatch");

        return "size=" + std::to_string(values.size());
    }

    template <typename Alloc>
    std::string exercise_unordered_map(size_t count, const Alloc& alloc) {
        std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, Alloc> values{
                0, std::hash<int>{}, std::equal_to<int>{}, alloc};
        values.reserve(count);

        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            auto [it, inserted] = values.emplace(static_cast<int>(i), static_cast<int>(i * 5 + 7));
            (void)it;
            require(inserted, "unordered_map emplace duplicate key");
        }

        require(values.size() == count, "unordered_map size mismatch");

        size_t stride = std::ranges::max(size_t{1}, count / 16);
        for (size_t i : std::ranges::iota_view{size_t{0}, count}) {
            if ((i % stride) != 0) {
                continue;
            }

            auto it = values.find(static_cast<int>(i));
            require(it != values.end(), "unordered_map find failed");
            require(it->second == static_cast<int>(i * 5 + 7), "unordered_map value mismatch");
        }

        size_t erase_count = count / 4;
        for (size_t i : std::ranges::iota_view{size_t{0}, erase_count}) {
            values.erase(static_cast<int>(i));
        }
        require(values.size() == (count - erase_count), "unordered_map erase size mismatch");

        return "size=" + std::to_string(values.size()) + " buckets=" + std::to_string(values.bucket_count());
    }

    template <typename Alloc>
    std::string exercise_string(size_t length, const Alloc& alloc) {
        std::basic_string<char, std::char_traits<char>, Alloc> s{alloc};
        s.reserve(length);

        for (size_t i : std::ranges::iota_view{size_t{0}, length}) {
            s.push_back(static_cast<char>('a' + (i % 26)));
        }

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

        for (size_t i : std::ranges::iota_view{size_t{0}, n}) {
            a.construct(p + i, make_value<T>(i));
        }

        for (size_t i : std::ranges::iota_view{size_t{0}, n}) {
            require(value_key(*(p + i)) == value_key(make_value<T>(i)), "allocate/construct round-trip mismatch");
            a.destroy(p + i);
        }

        a.deallocate(p, n);
        return "n=" + std::to_string(n);
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
            add_case(rows, memory_mode_to_string(Mode), "container_matrix", "n/a", 0, []() -> std::string {
                return "skipped (noheap rebind for list/deque/map/unordered_map not implemented)";
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

            for (size_t len : lengths) {
                add_mode_case<Mode, char>(rows, "string_push_back", len, char_budget, [&](const auto& alloc) {
                    return exercise_string(len, alloc);
                });
            }

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

        for (const auto& row : rows) {
            std::cout << std::setw(10) << row.mode << std::setw(28) << row.test_name << std::setw(16) << row.type_name
                      << std::setw(10) << row.workload << std::setw(8) << (row.pass ? "PASS" : "FAIL") << row.note
                      << "\n";
        }

        size_t pass_count = 0;
        for (const auto& row : rows) {
            if (row.pass) {
                ++pass_count;
            }
        }

        std::cout << "\nsummary: " << pass_count << "/" << rows.size() << " cases passed\n";
    }
}  // namespace

int main() {
    std::vector<smoke_row> rows;
    rows.reserve(64);

    add_mode_matrix<allocazam::memory_mode::dynamic>(rows);
    add_mode_matrix<allocazam::memory_mode::fixed>(rows);
    add_mode_matrix<allocazam::memory_mode::noheap>(rows);

    print_table(rows);

    for (const auto& row : rows) {
        if (!row.pass) {
            return 1;
        }
    }

    return 0;
}
