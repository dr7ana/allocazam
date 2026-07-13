#pragma once

#if defined(__linux__)

#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace allocazam_test {
    struct mapping_info {
        size_t size_kb{};
        size_t kernel_page_kb{};
        bool hugetlb{};
    };

    [[nodiscard]] inline bool parse_mapping_header(const std::string& line, uintptr_t& begin, uintptr_t& end) noexcept {
        size_t dash = line.find('-');
        size_t space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash >= space) {
            return false;
        }

        const char* data = line.data();
        auto [begin_end, begin_ec] = std::from_chars(data, data + dash, begin, 16);
        auto [end_end, end_ec] = std::from_chars(data + dash + 1, data + space, end, 16);
        return begin_ec == std::errc{} && end_ec == std::errc{} && begin_end == (data + dash) &&
               end_end == (data + space);
    }

    [[nodiscard]] inline size_t parse_kb_value(const std::string& line) noexcept {
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return 0;
        }

        const char* first = line.data() + colon + 1;
        const char* last = line.data() + line.size();
        while (first != last && *first == ' ') {
            ++first;
        }

        size_t value = 0;
        auto [end, ec] = std::from_chars(first, last, value);
        return ec == std::errc{} && end != first ? value : 0;
    }

    [[nodiscard]] inline bool vmflags_contains(const std::string& line, std::string_view needle) noexcept {
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }

        std::string_view rest{line.data() + colon + 1, line.size() - colon - 1};
        while (!rest.empty()) {
            size_t first = rest.find_first_not_of(' ');
            if (first == std::string_view::npos) {
                return false;
            }
            rest.remove_prefix(first);

            size_t end = rest.find(' ');
            if (rest.substr(0, end) == needle) {
                return true;
            }
            if (end == std::string_view::npos) {
                return false;
            }
            rest.remove_prefix(end + 1);
        }
        return false;
    }

    [[nodiscard]] inline std::optional<mapping_info> mapping_for_address(const void* pointer) {
        std::ifstream smaps{"/proc/self/smaps"};
        if (!smaps.is_open()) {
            return std::nullopt;
        }

        uintptr_t target = reinterpret_cast<uintptr_t>(pointer);
        std::string line;
        bool in_target = false;
        mapping_info info{};

        while (std::getline(smaps, line)) {
            uintptr_t begin = 0;
            uintptr_t end = 0;
            if (parse_mapping_header(line, begin, end)) {
                if (in_target) {
                    return info;
                }
                in_target = begin <= target && target < end;
                if (in_target) {
                    info = {};
                }
                continue;
            }

            if (!in_target) {
                continue;
            }

            std::string_view view{line};
            if (view.starts_with("Size:")) {
                info.size_kb = parse_kb_value(line);
            } else if (view.starts_with("KernelPageSize:")) {
                info.kernel_page_kb = parse_kb_value(line);
            } else if (view.starts_with("VmFlags:")) {
                info.hugetlb = vmflags_contains(line, "ht");
            }
        }

        return in_target ? std::optional<mapping_info>{info} : std::nullopt;
    }
}  // namespace allocazam_test

#endif
