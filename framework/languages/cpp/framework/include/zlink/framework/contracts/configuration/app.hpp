/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/configuration.hpp>
#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/configuration/framework_options.hpp>
#include <zlink/framework/contracts/configuration/logging.hpp>
#include <zlink/framework/contracts/configuration/module.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/eventing/health.hpp>
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

namespace zlink::framework
{

namespace detail
{
class app_state_t;
} // namespace detail

class app_t;

class app_advanced_t
{
  public:
    service_collection_t &services () noexcept;
    handler_registry_t &handlers () noexcept;
    zlink_builder_t &zlink () noexcept;

  private:
    friend class app_t;
    explicit app_advanced_t (app_t &app) noexcept;

    app_t *_app;
};

class app_t
{
  public:
    app_t ();
    ~app_t ();

    app_t (app_t &&) noexcept;
    app_t &operator= (app_t &&) noexcept;
    app_t (const app_t &) = delete;
    app_t &operator= (const app_t &) = delete;

    static app_t create ();

    config_builder_t &config () noexcept;
    logging_builder_t &logging () noexcept;
    health_builder_t &health () noexcept;
    app_advanced_t advanced () noexcept;

    // Turn message-flow tracing on/off (or change verbosity) at runtime, after the
    // app is built/running — for temporary diagnostics in production without a
    // restart. Reads live on every dispatch via a shared atomic. No-op until the
    // framework is applied. Thread-safe.
    app_t &set_message_flow_mode (message_flow_log_mode_t mode) noexcept;
    message_flow_log_mode_t message_flow_mode () const noexcept;

    app_t &add_module (module_t &module);
    app_t &add_zlink_framework (std::function<void (zlink_framework_options_t &)> configure);
    template <typename TModule, typename... TArgs>
    requires framework_module_contract_t<TModule> app_t &add_zlink_framework (TArgs &&...args)
    {
        TModule module (std::forward<TArgs> (args)...);
        module.configure_services (_services ());
        module.configure_zlink (_zlink_builder ());
        module.configure_handlers (_handlers ());
        return *this;
    }
    app_t &add_hosted_service (std::unique_ptr<hosted_service_t> service);

    int run (int argc, char **argv);

    task_t<relocation_result_t> relocate (
      relocation_options_t options,
      std::stop_token wait_cancellation = {});
    task_t<termination_result_t> shutdown (
      std::chrono::milliseconds deadline = std::chrono::seconds (30),
      std::stop_token wait_cancellation = {});
    framework_runtime_state_t runtime_state () const noexcept;
    bool is_ready () const noexcept;

    void stop () noexcept;
    void request_stop () noexcept;

  private:
    friend class app_advanced_t;

    static void run_shared_relocation (detail::app_state_t &state) noexcept;
    static void run_shared_shutdown (detail::app_state_t &state) noexcept;

    service_collection_t &_services () noexcept;
    handler_registry_t &_handlers () noexcept;
    zlink_builder_t &_zlink_builder () noexcept;
    serializer_registry_t &_serializers () noexcept;

    std::unique_ptr<detail::app_state_t> _state;
};

} // namespace zlink::framework
