/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

namespace zlink::framework::runtime
{
class runtime_failure_collector_t
{
  public:
    void bind_shutdown (std::function<void ()> shutdown) { _shutdown = std::move (shutdown); }
    void report (std::exception_ptr failure) noexcept
    {
        {
            std::lock_guard lock (_mutex);
            _failures.push_back (std::move (failure));
        }
        if (_shutdown)
            _shutdown ();
    }
    template <class Close> bool capture (Close &&close) noexcept
    {
        try {
            close ();
            return true;
        }
        catch (...) {
            report (std::current_exception ());
            return false;
        }
    }
    template <class... Resources> static void close_resources (Resources &...resources)
    {
        const auto close = [] (auto &resource) {
            if (!resource)
                return;
            resource->close ();
            resource.reset ();
        };
        (close (resources), ...);
    }
    template <class Close> void retain (Close close)
    {
        auto ownership = std::make_shared<Close> (std::move (close));
        std::lock_guard lock (_mutex);
        _retained.emplace_back ([ownership] { (*ownership) (); });
    }
    void close_retained () noexcept
    {
        std::vector<std::function<void ()>> resources;
        {
            std::lock_guard lock (_mutex);
            resources.swap (_retained);
        }
        for (auto &resource : resources) {
            if (!capture (resource)) {
                std::lock_guard lock (_mutex);
                _retained.push_back (std::move (resource));
            }
        }
    }
    void rethrow_if_failed () const
    {
        std::exception_ptr failure;
        {
            std::lock_guard lock (_mutex);
            if (!_failures.empty ())
                failure = _failures.front ();
        }
        if (failure)
            std::rethrow_exception (failure);
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::exception_ptr> _failures;
    std::vector<std::function<void ()>> _retained;
    std::function<void ()> _shutdown;
};
}
