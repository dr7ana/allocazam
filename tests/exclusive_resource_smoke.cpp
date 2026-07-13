#include "exclusive_resource.hpp"
#include "utils.hpp"

#include <array>
#include <csignal>
#include <iostream>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ALLOCAZAM_TEST_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define ALLOCAZAM_TEST_TSAN 1
#endif

#if !defined(NDEBUG) && defined(__linux__) && !defined(ALLOCAZAM_TEST_TSAN)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
    using allocazam::detail::exclusive_resource;

    template <typename Fn>
    void require_invalid_argument(Fn&& fn, std::string_view message) {
        bool threw = false;
        try {
            fn();
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, message);
    }

    void test_constructor_validation() {
        alignas(64) std::array<std::byte, 256> backing{};
        require_invalid_argument(
                [&] { exclusive_resource resource{nullptr, backing.size(), 64}; },
                "null exclusive base must be rejected");
        require_invalid_argument(
                [&] { exclusive_resource resource{backing.data(), 0, 64}; },
                "zero exclusive capacity must be rejected");
        require_invalid_argument(
                [&] { exclusive_resource resource{backing.data(), backing.size(), 3}; },
                "non-power-of-two exclusive alignment must be rejected");
        require_invalid_argument(
                [&] { exclusive_resource resource{backing.data() + 1, backing.size() - 1, 64}; },
                "misaligned exclusive base must be rejected");
    }

    void test_claim_release_and_ownership() {
        alignas(64) std::array<std::byte, 256> backing{};
        exclusive_resource resource{backing.data(), backing.size(), 64};

        require(!resource.claimed(), "new exclusive resource must be unclaimed");
        require(resource.capacity_bytes() == backing.size(), "exclusive capacity changed");
        require(resource.alignment() == 64, "exclusive alignment changed");
        require(resource.owns(backing.data()), "exclusive resource must own its exact base");
        require(!resource.owns(backing.data() + 1), "exclusive resource must reject an interior pointer");
        require(!resource.owns(nullptr), "exclusive resource must reject null ownership");

        require(resource.claim(0, 1, 1) == nullptr, "zero-minimum claim must fail");
        require(resource.claim(32, 16, 1) == nullptr, "inverted claim interval must fail");
        require(resource.claim(1, backing.size() + 1, 1) == nullptr, "oversized claim must fail");
        require(resource.claim(1, 1, 128) == nullptr, "over-aligned claim must fail");
        require(resource.claim(1, 1, 3) == nullptr, "invalid claim alignment must fail");

        void* pointer = resource.claim(32, 96, 32);
        require(pointer == backing.data(), "exclusive claim did not return exact base");
        require(resource.claimed(), "successful exclusive claim was not recorded");
        require(resource.claim(1, 1, 1) == nullptr, "second exclusive claim must fail");

        resource.release(pointer, 32, 32);
        require(!resource.claimed(), "minimum-size release did not clear claim");

        pointer = resource.claim(32, 96, 32);
        resource.release(pointer, 64, 32);
        require(!resource.claimed(), "intermediate-size release did not clear claim");

        pointer = resource.claim(32, 96, 32);
        resource.release(pointer, 96, 32);
        require(!resource.claimed(), "maximum-size release did not clear claim");
    }

    void test_expand_and_payload_retention() {
        alignas(64) std::array<std::byte, 256> backing{};
        backing.fill(std::byte{0x5a});
        exclusive_resource resource{backing.data(), backing.size(), 64};

        require(resource.expand(backing.data(), 1, backing.size()) == 0, "unclaimed expand must return zero");
        void* pointer = resource.claim(32, 32, 16);
        require(pointer != nullptr, "expand setup claim failed");
        require(resource.expand(nullptr, 64, backing.size()) == 0, "null expand must return zero");
        require(resource.expand(backing.data() + 1, 64, backing.size()) == 0, "interior expand must return zero");
        require(resource.expand(pointer, 16, backing.size()) == 32, "no-op expand changed current maximum");
        require(resource.expand(pointer, backing.size() + 1, backing.size()) == 32,
                "oversized expand changed current maximum");
        require(resource.expand(pointer, 64, backing.size()) == backing.size(),
                "valid expand did not publish full capacity");

        for (std::byte value : backing) {
            require(value == std::byte{0x5a}, "exclusive bookkeeping modified payload backing");
        }

        resource.release(pointer, backing.size(), 16);
        require(!resource.claimed(), "expanded release did not clear claim");
    }

#if !defined(NDEBUG) && defined(__linux__) && !defined(ALLOCAZAM_TEST_TSAN)
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

    void test_debug_contract_assertions() {
        require(expect_abort([] {
                    alignas(64) std::array<std::byte, 256> backing{};
                    exclusive_resource resource{backing.data(), backing.size(), 64};
                    void* pointer = resource.claim(32, 96, 32);
                    resource.release(static_cast<std::byte*>(pointer) + 1, 32, 32);
                }),
                "interior exclusive release must abort");
        require(expect_abort([] {
                    alignas(64) std::array<std::byte, 256> backing{};
                    exclusive_resource resource{backing.data(), backing.size(), 64};
                    void* pointer = resource.claim(32, 96, 32);
                    resource.release(pointer, 31, 32);
                }),
                "below-minimum exclusive release must abort");
        require(expect_abort([] {
                    alignas(64) std::array<std::byte, 256> backing{};
                    exclusive_resource resource{backing.data(), backing.size(), 64};
                    void* pointer = resource.claim(32, 96, 32);
                    resource.release(pointer, 97, 32);
                }),
                "above-maximum exclusive release must abort");
        require(expect_abort([] {
                    alignas(64) std::array<std::byte, 256> backing{};
                    exclusive_resource resource{backing.data(), backing.size(), 64};
                    void* pointer = resource.claim(32, 96, 32);
                    resource.release(pointer, 32, 16);
                }),
                "wrong-alignment exclusive release must abort");
        require(expect_abort([] {
                    alignas(64) std::array<std::byte, 256> backing{};
                    exclusive_resource resource{backing.data(), backing.size(), 64};
                    (void)resource.claim(32, 32, 32);
                }),
                "exclusive resource destruction with live claim must abort");
    }
#elif defined(NDEBUG)
    void test_release_fail_closed() {
        alignas(64) std::array<std::byte, 256> backing{};
        exclusive_resource resource{backing.data(), backing.size(), 64};
        void* pointer = resource.claim(32, 96, 32);
        require(pointer != nullptr, "release validation setup failed");

        resource.release(static_cast<std::byte*>(pointer) + 1, 32, 32);
        require(resource.claimed(), "invalid pointer released exclusive claim");
        resource.release(pointer, 31, 32);
        require(resource.claimed(), "below-minimum size released exclusive claim");
        resource.release(pointer, 97, 32);
        require(resource.claimed(), "above-maximum size released exclusive claim");
        resource.release(pointer, 32, 16);
        require(resource.claimed(), "wrong alignment released exclusive claim");

        resource.release(pointer, 32, 32);
        require(!resource.claimed(), "valid release did not clear claim");
    }
#else
    void test_sanitized_debug_build() {
        // Invalid-release death cases use fork and are covered by ordinary Debug CI.
        // TSan still exercises every valid claim/release path in this executable.
    }
#endif
}  // namespace

int main() {
    try {
        test_constructor_validation();
        test_claim_release_and_ownership();
        test_expand_and_payload_retention();
#if !defined(NDEBUG) && defined(__linux__) && !defined(ALLOCAZAM_TEST_TSAN)
        test_debug_contract_assertions();
#elif defined(NDEBUG)
        test_release_fail_closed();
#else
        test_sanitized_debug_build();
#endif
    } catch (const std::exception& e) {
        std::cerr << "exclusive resource smoke failed: " << e.what() << '\n';
        return 1;
    }

    std::cout << "exclusive resource smoke: all tests passed\n";
    return 0;
}

#if defined(ALLOCAZAM_TEST_TSAN)
#undef ALLOCAZAM_TEST_TSAN
#endif
