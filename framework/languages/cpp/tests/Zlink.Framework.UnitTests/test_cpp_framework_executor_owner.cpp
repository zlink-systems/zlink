/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>
#include "runtime/dispatch/coroutine_executor.hpp"

#include <memory>
#include <future>
#include <cassert>
#include <optional>

namespace
{

zlink::framework::task_t<int> await_late_child (zlink::framework::task_t<int> child)
{
    co_return co_await child;
}

class stop_on_start_t final : public zlink::framework::hosted_service_t
{
  public:
    stop_on_start_t (zlink::framework::app_t &app,
                     zlink::framework::task_completion_source_t<int> &child,
                     std::optional<zlink::framework::task_t<int>> &waiter) :
        _app (app), _child (child), _waiter (waiter)
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        namespace detail = zlink::framework::detail;
        detail::completion_dispatcher_borrow_t first{}, second{};
        assert (detail::capture_runtime_native_continuation_scheduler (&first));
        assert (detail::capture_runtime_native_continuation_scheduler (&second));
        assert (first && second);
        assert (first == second);
        // This wait has no application owner to cancel it during run shutdown.
        const auto previous = detail::exchange_wait_owner ({});
        _waiter.emplace (await_late_child (_child.task ()));
        detail::exchange_wait_owner (previous);
        _app.stop ();
        co_return;
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t &_app;
    zlink::framework::task_completion_source_t<int> &_child;
    std::optional<zlink::framework::task_t<int>> &_waiter;
};

} // namespace

int main ()
{
    // Standalone handler execution must not retain a dispatcher until static
    // executor destruction. Provider registrations own their completion turns.
    assert (!zlink::framework::runtime::handler_coroutine_executor ().completion_dispatcher ());
    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    {
        auto configured = zlink::framework::app_t::create ();
        configured.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &) {});
    }

    auto running = zlink::framework::app_t::create ();
    running.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &) {});
    zlink::framework::task_completion_source_t<int> child;
    std::optional<zlink::framework::task_t<int>> waiter;
    running.add_hosted_service (std::make_unique<stop_on_start_t> (running, child, waiter));

    if (running.run (0, nullptr) != 0) {
        return 1;
    }
    child.complete (zlink::framework::result_t<int>::success (42));
    if (!waiter || !waiter->await_ready ())
        return 4;
    const auto rejected = waiter->result ();
    if (rejected
        || rejected.error_kind () != zlink::framework::framework_error_kind_t::shutting_down)
        return 5;
    zlink::framework::detail::completion_dispatcher_borrow_t dispatcher{};
    if (zlink::framework::detail::capture_runtime_native_continuation_scheduler (&dispatcher))
        return 2;
    if (dispatcher)
        return 3;

    // A captured handoff remains late-bound across stop/start, even before
    // the replacement host has captured its first continuation.
    namespace runtime = zlink::framework::runtime;
    zlink::framework::completion_owner_t owner;
    runtime::configure_handler_coroutine_executor (2);
    auto scheduler = zlink::framework::detail::capture_runtime_native_continuation_scheduler ();
    assert (scheduler);
    runtime::shutdown_handler_coroutine_executor ();
    runtime::configure_handler_coroutine_executor (2);
    std::promise<void> resumed;
    auto result = resumed.get_future ();
    scheduler ([&] { resumed.set_value (); });
    result.get ();
    runtime::shutdown_handler_coroutine_executor ();
    return 0;
}
