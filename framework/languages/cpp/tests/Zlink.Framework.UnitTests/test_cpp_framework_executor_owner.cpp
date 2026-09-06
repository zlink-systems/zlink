/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include <memory>

namespace
{

class stop_on_start_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit stop_on_start_t (zlink::framework::app_t &app) : _app (app) {}

    zlink::framework::task_t<void>
    start (zlink::framework::service_provider_t &) override
    {
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
    {
        auto configured = zlink::framework::app_t::create ();
        configured.add_zlink_framework (
          [] (zlink::framework::zlink_framework_options_t &) {});
    }

    auto running = zlink::framework::app_t::create ();
    running.add_zlink_framework (
      [] (zlink::framework::zlink_framework_options_t &) {});
    running.add_hosted_service (
      std::make_unique<stop_on_start_t> (running));

    if (running.run (0, nullptr) != 0) {
        return 1;
    }
    return zlink::framework::detail::
             capture_runtime_native_continuation_scheduler ()
             ? 2
             : 0;
}
