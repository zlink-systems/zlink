#ifndef PERF_MULTI_CLIENT_HELPERS_HPP
#define PERF_MULTI_CLIENT_HELPERS_HPP

#include "perf_common.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace perf
{
namespace multi
{

// Echo clients keep their sockets open until every admitted record has been
// received. This counts teardown work only; it never gates active sends.
class echo_reply_drain_t
{
  public:
    // Count before awaiting: receive dispatch can precede the scheduled send
    // continuation even though Core has already admitted the record.
    void submitted () { ++_pending; }

    bool finished ()
    {
        if (_pending == 0) {
            errno = EPROTO;
            return false;
        }
        --_pending;
        return true;
    }

    template <typename TDispatch>
    bool drain (zlink::poller_t &poller,
                size_t source_count,
                std::chrono::steady_clock::time_point deadline,
                TDispatch &&dispatch)
    {
        std::vector<zlink::poll_event_t> events (source_count);
        while (_pending != 0) {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return false;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              deadline - now);
            const size_t ready = poller.wait (
              events.data (), events.size (),
              std::chrono::milliseconds (std::max<int64_t> (1, remaining.count ())));
            if (ready != 0 && !dispatch (events.data (), ready))
                return false;
        }
        return true;
    }

  private:
    size_t _pending = 0;
};

inline std::string parse_endpoint_arg (int argc, char **argv)
{
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp (argv[i], "--endpoint") == 0)
            return argv[i + 1];
    }
    return std::string ();
}

} // namespace multi
} // namespace perf

#endif
