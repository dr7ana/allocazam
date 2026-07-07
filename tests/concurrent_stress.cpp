#include "allocazam.hpp"
#include "utils.hpp"

#include <barrier>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// test case 1 from the thread-caching plan: N threads churning through
// default-constructed allocators over all three routes — node containers (pool via
// stateless rebind), string traffic (TLS run cache fronting the runner), and direct
// allocate(1) (pool) — plus cross-thread frees over the remote stacks. deterministic
// value checks catch corruption the canaries can see; for runner corruption TSan is the
// only detector, which is why CI runs this binary under -fsanitize=thread.

namespace {
    template <typename T>
    using dyn_alloc = allocazam::allocazam_std_allocator<T, allocazam::memory_mode::dynamic>;

    using pair_alloc = dyn_alloc<std::pair<const int, int>>;
    using map_t = std::map<int, int, std::less<int>, pair_alloc>;
    using umap_t = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, pair_alloc>;
    using string_t = std::basic_string<char, std::char_traits<char>, dyn_alloc<char>>;

    struct mailbox {
        std::mutex guard;
        std::vector<int*> ptrs;
    };

    void worker_pass(size_t tid, size_t threads, size_t rounds, std::vector<mailbox>& boxes) {
        dyn_alloc<int> ints{};

        for (size_t round : std::views::iota(size_t{0}, rounds)) {
            // node-container churn: rebound stateless allocators -> the thread's pools
            map_t values{pair_alloc{}};
            umap_t hashed{0, std::hash<int>{}, std::equal_to<int>{}, pair_alloc{}};
            for (int i : std::views::iota(0, 64)) {
                values.emplace(i, i * 3 + static_cast<int>(tid));
                hashed.emplace(i, i * 5 + static_cast<int>(tid));
            }
            require(values.size() == 64 && hashed.size() == 64, "container churn size mismatch");
            require(values.at(63) == 189 + static_cast<int>(tid), "map value mismatch");
            require(hashed.at(63) == 315 + static_cast<int>(tid), "unordered_map value mismatch");

            // string traffic: TLS run cache fronting the runner
            string_t s{dyn_alloc<char>{}};
            for (size_t i : std::views::iota(size_t{0}, size_t{512})) {
                s.push_back(static_cast<char>('a' + (i % 26)));
            }
            require(s.size() == 512 && s.front() == 'a', "string churn mismatch");

            // direct pool traffic + cross-thread handoff: this thread's node goes to the
            // next thread; nodes from the previous thread are freed here (remote route)
            int* p = ints.allocate(1);
            require(p != nullptr, "direct allocate failed");
            *p = static_cast<int>(round);

            {
                mailbox& out = boxes[(tid + 1) % threads];
                std::lock_guard lock{out.guard};
                out.ptrs.push_back(p);
            }
            {
                mailbox& in = boxes[tid];
                std::vector<int*> grabbed;
                {
                    std::lock_guard lock{in.guard};
                    grabbed.swap(in.ptrs);
                }
                std::ranges::for_each(grabbed, [&](int* q) { ints.deallocate(q, 1); });
            }
        }
    }
}  // namespace

int main(int argc, char** argv) {
    size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 4;
    }
    threads = std::ranges::min(threads, size_t{8});
    size_t rounds = 200;

    if (argc > 1) {
        threads = std::ranges::max(size_t{1}, static_cast<size_t>(std::strtoull(argv[1], nullptr, 10)));
    }
    if (argc > 2) {
        rounds = std::ranges::max(size_t{1}, static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)));
    }

    std::vector<mailbox> boxes(threads);
    std::barrier<> start{static_cast<std::ptrdiff_t>(threads)};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    std::vector<std::string> errors(threads);
    for (size_t tid : std::views::iota(size_t{0}, threads)) {
        workers.emplace_back([&, tid] {
            start.arrive_and_wait();
            try {
                worker_pass(tid, threads, rounds, boxes);
            } catch (const std::exception& e) {
                errors[tid] = e.what();
            }
        });
    }
    std::ranges::for_each(workers, [](std::thread& t) { t.join(); });

    for (size_t tid : std::views::iota(size_t{0}, threads)) {
        if (!errors[tid].empty()) {
            std::cout << "[FAIL] concurrent stress thread " << tid << ": " << errors[tid] << "\n";
            return 1;
        }
    }

    // leftover handoffs: freed from the main thread, landing on remote stacks
    dyn_alloc<int> ints{};
    size_t leftover = 0;
    std::ranges::for_each(boxes, [&](mailbox& box) {
        leftover += box.ptrs.size();
        std::ranges::for_each(box.ptrs, [&](int* p) { ints.deallocate(p, 1); });
        box.ptrs.clear();
    });

    std::cout << "concurrent stress: " << threads << " threads x " << rounds << " rounds passed (leftover=" << leftover
              << ")\n";
    return 0;
}
