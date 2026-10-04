/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once
#include <exception>
#include <stdexcept>
#include <string>
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
    void bind_runtime_reporter (std::function<void (std::exception_ptr)> reporter)
    {
        std::lock_guard lock (_mutex);
        _runtime_reporter = std::move (reporter);
    }
    void report (std::exception_ptr failure) noexcept
    {
        std::function<void (std::exception_ptr)> reporter;
        {
            std::lock_guard lock (_mutex);
            reporter = _runtime_reporter;
            if (!reporter)
                _failures.push_back (failure);
        }
        if (reporter)
            reporter (std::move (failure));
    }
    template <class Close> bool capture (Close &&close) noexcept
    {
        try {
            close ();
            return true;
        }
        catch (const std::exception &) {
            report (std::current_exception ());
            return false;
        }
    }
    template <class... Resources> static void close_resources (Resources &...resources)
    {
        runtime_failure_collector_t failures;
        const auto close = [&failures] (auto &resource) {
            if (resource)
                failures.capture ([&] {
                    resource->close ();
                    resource.reset ();
                });
        };
        (close (resources), ...);
        failures.rethrow_if_failed ();
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
        std::vector<std::exception_ptr> failures;
        {
            std::lock_guard lock (_mutex);
            failures = _failures;
        }
        if (failures.size () == 1)
            std::rethrow_exception (failures.front ());
        if (!failures.empty ())
            throw cleanup_failure_t (std::move (failures));
    }

  private:
    class cleanup_failure_t final : public std::runtime_error
    {
      public:
        explicit cleanup_failure_t (std::vector<std::exception_ptr> failures) :
            std::runtime_error (message (failures)), _failures (std::move (failures))
        {
        }

      private:
        static std::string message (const std::vector<std::exception_ptr> &failures)
        {
            std::string text;
            for (const auto &failure : failures) {
                try {
                    std::rethrow_exception (failure);
                }
                catch (const std::exception &error) {
                    if (!text.empty ())
                        text += "; ";
                    text += error.what ();
                }
            }
            return text;
        }
        std::vector<std::exception_ptr> _failures;
    };
    mutable std::mutex _mutex;
    std::function<void (std::exception_ptr)> _runtime_reporter;
    std::vector<std::exception_ptr> _failures;
    std::vector<std::function<void ()>> _retained;
};
}
