/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_REQUEST_PROGRESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_REQUEST_PROGRESS_HPP_INCLUDED

#include <zlink.h>

#include <functional>
#include <memory>

namespace zlink
{
namespace detail
{

class request_progress_poller_t
{
  public:
    explicit request_progress_poller_t (void *handle_) noexcept :
        poller (zlink_poller_new ()), registered (false)
    {
        if (!poller || !handle_)
            return;
        registered =
          zlink_poller_add (poller, handle_, nullptr, ZLINK_POLLCOMPLETION) == ZLINK_CONFIG_OK;
    }

    ~request_progress_poller_t ()
    {
        if (poller)
            (void) zlink_poller_destroy (&poller);
    }

    request_progress_poller_t (const request_progress_poller_t &) = delete;
    request_progress_poller_t &operator= (const request_progress_poller_t &) = delete;

    void poll_once () noexcept
    {
        if (!registered)
            return;
        zlink_poller_event_t event;
        (void) zlink_poller_wait (poller, &event, 1, 0, nullptr);
    }

  private:
    void *poller;
    bool registered;
};

inline std::function<void ()> make_request_progress_callback (void *handle_)
{
    auto progress = std::make_shared<request_progress_poller_t> (handle_);
    return [progress] () { progress->poll_once (); };
}

} // namespace detail
} // namespace zlink

#endif
