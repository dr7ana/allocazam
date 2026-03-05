#pragma once

#include "allocazam.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct test_object {
    int x;
    int y;
    int z;

    constexpr test_object(int x, int y, int z) : x(x), y(y), z(z) {}
    ~test_object() = default;
};

inline constexpr void require(bool condition, std::string_view msg) {
    if (!condition) {
        throw std::runtime_error{std::string{msg}};
    }
}

inline constexpr std::string_view memory_mode_to_string(allocazam::memory_mode mode) noexcept {
    using namespace std::string_view_literals;
    switch (mode) {
        case allocazam::memory_mode::fixed:
            return "fixed"sv;
        case allocazam::memory_mode::dynamic:
            return "dynamic"sv;
        case allocazam::memory_mode::noheap:
            return "noheap"sv;
    }
    return "unknown"sv;
}
