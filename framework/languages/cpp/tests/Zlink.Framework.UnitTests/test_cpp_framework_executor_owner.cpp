/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>
#include "runtime/dispatch/coroutine_executor.hpp"

#include <memory>
#include <future>
#include <cassert>

namespace
{

class stop_on_start_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit stop_on_start_t (zlink::framework::app_t &app) : _app (app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        namespace detail = zlink::framework::detail;
        detail::completion_dispatcher_borrow_t first{}, second{};
        assert (detail::capture_runtime_native_continuation_scheduler (&first));
        assert (detail::capture_runtime_native_continuation_scheduler (&second));
        assert (first && second);
        assert (first == second);
        _app.stop ();
        co_return;
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t &_app;
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
    running.add_hosted_service (std::make_unique<stop_on_start_t> (running));

    if (running.run (0, nullptr) != 0) {
        return 1;
    }
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
