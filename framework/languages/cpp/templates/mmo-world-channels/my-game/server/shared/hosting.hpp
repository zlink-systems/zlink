#pragma once
#include "config.hpp"
#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>
#include <iostream>
#include <thread>

namespace MyGame
{
namespace fx = zlink::framework;
template <class T> T await_host (fx::task_t<T> task, std::chrono::milliseconds deadline)
{
    const auto result = task.result_for (deadline);
    if (!result)
        throw std::runtime_error ("Host operation deadline expired");
    return result->value ();
}
template <class Status> bool safe_to_shutdown (const Status &status)
{
    if constexpr (requires { status.safe_to_shutdown; })
        return status.safe_to_shutdown;
    else
        throw std::runtime_error (
          "Framework public SafeToShutdown observation is unavailable (#1740)");
}
class NodeControl final : public fx::hosted_service_t
{
    fx::app_t &app_;
    const NodeConfig &config_;
    bool placement_;
    std::thread control_;
    bool failed_ = false;

  public:
    NodeControl (fx::app_t &app, const NodeConfig &config, bool placement) :
        app_ (app), config_ (config), placement_ (placement)
    {
    }
    bool failed () const { return failed_; }
    fx::task_t<void> start (fx::service_provider_t &services) override
    {
        auto &meshes = services.get_required<fx::route_mesh_runtime_t> ();
        auto ready = std::make_shared<fx::task_completion_source_t<void>> ();
        auto observation = meshes.observe (
          Names::mesh (config_.WorldId), Limits::observation_capacity,
          [ready, placement = placement_] (const auto &observed) {
              if (observed.loss.discarded_terminal_count != 0)
                  ready->complete (fx::result_t<void>::failure (
                    fx::framework_error_kind_t::internal_failure, "Readiness observation lost"));
              else if (observed.status.is_ready
                       && (!placement || observed.status.placement.is_available))
                  ready->complete (fx::result_t<void>::success ());
          });
        co_await ready->task ();
        observation->close ();
        if (!config_.ZoneIds.empty ()) {
            auto &spots = services.get_required<fx::spot_manager_t> ();
            for (const auto &id : config_.ZoneIds) {
                const auto created = co_await spots.get_or_create (id, Names::zone)
                                       .in_mesh (Names::mesh (config_.WorldId))
                                       .async ();
                if (created.state == fx::spot_create_state_t::rejected)
                    throw std::runtime_error ("Zone bootstrap rejected: " + id);
            }
        }
        auto &runtime = services.get_required<fx::framework_runtime_t> ();
        nlohmann::json output{
          {"id", config_.Id},
          {"meshEndpoint",
           runtime.listener_status (fx::listener_kind_t::route_mesh, Names::mesh (config_.WorldId))
             .endpoint}};
        output["streamEndpoint"] =
          config_.StreamEndpoint
            ? nlohmann::json (
                runtime.listener_status (fx::listener_kind_t::stream, Names::stream).endpoint)
            : nlohmann::json (nullptr);
        std::cout << Control::ready << output.dump () << std::endl;
        control_ = std::thread ([this, &runtime] {
            try {
                std::string command;
                while (std::getline (std::cin, command)) {
                    if (command == Control::drain) {
                        std::cout << Control::requested << std::endl;
                        fx::relocation_options_t options;
                        options.mode = fx::relocation_mode_t::planned_maintenance;
                        options.deadline = Limits::relocation;
                        const auto relocated =
                          await_host (app_.relocate (options), Limits::relocation);
                        if (relocated.outcome != fx::relocation_outcome_t::relocated)
                            throw std::runtime_error (
                              "Host relocation blocked: "
                              + std::to_string (static_cast<int> (relocated.reason)));
                        std::cout << Control::relocated << std::endl;
                        // Availability of this public observation is required; state is not inferred.
                        (void) safe_to_shutdown (runtime.status ());
                        auto safe = std::make_shared<fx::task_completion_source_t<void>> ();
                        auto observation = runtime.observe (
                          Limits::observation_capacity, [safe] (const auto &observed) {
                              if (observed.loss.discarded_terminal_count != 0)
                                  safe->complete (fx::result_t<void>::failure (
                                    fx::framework_error_kind_t::internal_failure,
                                    "Lifecycle observation lost"));
                              else if (safe_to_shutdown (observed.status))
                                  safe->complete (fx::result_t<void>::success ());
                          });
                        await_host (safe->task (), Limits::safe);
                        observation->close ();
                        std::cout << Control::safe << std::endl;
                    } else if (command == Control::shutdown) {
                        const auto stopped =
                          await_host (app_.shutdown (Limits::shutdown), Limits::shutdown);
                        if (stopped.outcome != fx::termination_outcome_t::stopped
                            || stopped.reason != fx::termination_reason_t::none)
                            throw std::runtime_error (
                              "Host shutdown failed: "
                              + std::to_string (static_cast<int> (stopped.reason)));
                        std::cout << Control::stopped << std::endl;
                        app_.request_stop ();
                        return;
                    } else
                        throw std::invalid_argument ("Unknown node command: " + command);
                }
                throw std::runtime_error ("Node control input closed before shutdown");
            }
            catch (const std::exception &error) {
                failed_ = true;
                std::cerr << "Node operation failed: " << error.what () << std::endl;
                app_.request_stop ();
            }
        });
    }
    void stop () noexcept override {}
    void join_control ()
    {
        if (control_.joinable ())
            control_.join ();
    }
};
inline fx::zlink_framework_options_t &configure_host (fx::app_t &app, const NodeConfig &config)
{
    app.logging ()
      .set_min_level (fx::log_level_t::trace)
      .use_callback_sink ([] (const fx::log_record_t &record) {
          nlohmann::json line{{"category", record.category}, {"message", record.message}};
          for (const auto &field : record.fields)
              line[field.key] = field.value;
          std::cerr << line.dump () << std::endl;
      });
    auto &options = app.add_zlink_framework ();
    options.services ().add_singleton (std::make_unique<NodeConfig> (config));
    options.set_message_follow_duration (Limits::message_follow);
    options.configure_dispatch ().message_flow (fx::message_flow_log_mode_t::normal);
    options.add_location_store<fx::redis::redis_location_store_t> ()
      .set_connection_string (config.RedisEndpoint)
      .set_key_prefix (config.KeyPrefix + "location:");
    options.add_relocation_store<fx::redis::redis_relocation_store_t> ()
      .set_connection_string (config.RedisEndpoint)
      .set_key_prefix (config.KeyPrefix + "relocation:");
    return options;
}
inline int
run_host (fx::app_t &app, const NodeConfig &config, bool placement, int argc, char **argv)
{
    auto control = std::make_unique<NodeControl> (app, config, placement);
    auto *result = control.get ();
    app.add_hosted_service (std::move (control));
    const auto code = app.run (argc, argv);
    result->join_control ();
    return result->failed () ? 1 : code;
}
}
