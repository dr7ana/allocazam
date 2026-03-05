#include "allocazam.hpp"

#if defined(_SC_PAGESIZE)
#include <unistd.h>
#endif

namespace allocazam {
    //

    namespace detail {
        //
        size_t detect_page_size() noexcept {
            static size_t cached_page_size = [] {
#if defined(_SC_PAGESIZE)
                long p = ::sysconf(_SC_PAGESIZE);
                if (p > 0) {
                    return static_cast<size_t>(p);
                }
#endif
                // default 4KiB
                return size_t{4096};
            }();

            return cached_page_size;
        }
    }  // namespace detail
}  // namespace allocazam
