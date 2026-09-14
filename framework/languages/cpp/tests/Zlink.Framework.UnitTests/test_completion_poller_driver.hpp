/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <thread>

namespace zlink::framework::test
{

// Direct binding fixtures do not have a Framework receive loop. This driver
// gives those fixture sockets the explicit public-poller completion owner that
// production runtimes provide.
class completion_poller_driver_t
{
  public:
    template <typename Socket>
    explicit completion_poller_driver_t (Socket &socket)
    {
        _poller.add (socket, zlink::poll_event_flag_t::pollcompletion, 1);
        _driver = std::thread ([this] { drive (); });
    }

    ~completion_poller_driver_t ()
    {
        _stopping.store (true, std::memory_order_release);
        if (_driver.joinable ())
            _driver.join ();
        _poller.close ();
        assert (!_failure);
    }

    completion_poller_driver_t (const completion_poller_driver_t &) = delete;
    completion_poller_driver_t &operator= (const completion_poller_driver_t &) = delete;

  private:
    void drive () noexcept
    {
        try {
            while (!_stopping.load (std::memory_order_acquire)) {
                zlink::poll_event_t event;
                (void) _poller.wait (&event, 1, std::chrono::milliseconds (25));
            }
        }
        catch (...) {
            _failure = std::current_exception ();
        }
    }

    zlink::poller_t _poller;
    std::thread _driver;
    std::atomic_bool _stopping{false};
    std::exception_ptr _failure;
};

} // namespace zlink::framework::test
