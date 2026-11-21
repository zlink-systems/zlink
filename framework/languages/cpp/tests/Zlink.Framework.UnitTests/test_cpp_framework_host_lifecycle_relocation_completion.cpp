/* SPDX-License-Identifier: Elastic-2.0 */

#include "host_lifecycle_tests.hpp"

#include <zlink/json.hpp>

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../support/host_lifecycle_relocation_ready_fixture.hpp"

#include "../support/host_lifecycle_wait.hpp"

namespace zlink::framework::test::host_lifecycle
{
constexpr auto host_relocation_mesh_name = "host-relocation-mesh";

class relocation_source_service_t final : public zlink::framework::hosted_service_t
{
  public:
    relocation_source_service_t (zlink::framework::app_t &app,
                                 std::shared_ptr<std::atomic_bool> submit_ready) :
        _app (&app), _submit_ready (std::move (submit_ready))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto manager = services.get_required<zlink::framework::spot_manager_t> ();
        auto client = _app->advanced ().zlink ().route_client (
          services.get_required<zlink::framework::serializer_registry_t> ());
        _sender = std::thread (
          [this, manager = std::move (manager), client = std::move (client)] () mutable {
              const auto create_deadline =
                std::chrono::steady_clock::now () + std::chrono::seconds (5);
              std::string last_create_error;
              while (!_stop.load (std::memory_order_acquire)
                     && std::chrono::steady_clock::now () < create_deadline) {
                  const auto created =
                    manager
                      .get_or_create (zlink::framework::spot_id_t ("host-relocation-spot"),
                                      "host-relocation-spot")
                      .in_mesh (host_relocation_mesh_name)
                      .timeout (std::chrono::seconds (1))
                      .async ()
                      .result ();
                  if (created) {
                      created_spot.store (true, std::memory_order_release);
                      break;
                  }
                  if (last_create_error.empty ())
                      last_create_error =
                        created.error () ? created.error ()->what () : "Spot create failed";
                  std::this_thread::sleep_for (std::chrono::milliseconds (10));
              }
              if (!created_spot.load (std::memory_order_acquire)) {
                  error = std::move (last_create_error);
                  return;
              }
              while (!_stop.load (std::memory_order_acquire)
                     && !_submit_ready->load (std::memory_order_acquire))
                  std::this_thread::sleep_for (std::chrono::milliseconds (1));
              if (_stop.load (std::memory_order_acquire))
                  return;
              const auto submitted =
                client
                  .send_to_spot (zlink::framework::spot_id_t ("host-relocation-spot"),
                                 relocation_ready_message_t{1})
                  .async ()
                  .result ();
              if (submitted)
                  ready_sent.store (true, std::memory_order_release);
              else
                  error = submitted.error () ? submitted.error ()->what ()
                                             : "readiness message submission failed";
          });
        co_return;
    }

    void stop () noexcept override
    {
        _stop.store (true, std::memory_order_release);
        _submit_ready->store (true, std::memory_order_release);
        if (_sender.joinable ())
            _sender.join ();
    }

    std::atomic_bool created_spot{false};
    std::atomic_bool ready_sent{false};
    std::string error;

  private:
    zlink::framework::app_t *_app;
    std::shared_ptr<std::atomic_bool> _submit_ready;
    std::atomic_bool _stop{false};
    std::thread _sender;
};

class relocation_ready_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<relocation_ready_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (relocation_ready_spot_t &,
                                                              std::stop_token) override
    {
        if (capture_release) {
            capture_entered->complete (zlink::framework::result_t<void>::success ());
            co_await capture_release->task ();
        }
        co_return std::vector<std::byte>{std::byte{0x51}, std::byte{0x52}};
    }

    zlink::framework::task_t<void>
    restore (relocation_ready_spot_t &, std::vector<std::byte> payload, std::stop_token) override
    {
        restored.store (payload == std::vector<std::byte>{std::byte{0x51}, std::byte{0x52}},
                        std::memory_order_release);
        co_return;
    }

    static inline std::atomic_bool restored{false};
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>> capture_entered;
    static inline std::shared_ptr<zlink::framework::task_completion_source_t<void>> capture_release;
};

void configure_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> &location_store,
  const std::shared_ptr<zlink::framework::runtime::in_memory_relocation_store_t> &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework ([location_store, relocation_store,
                              routing_id = std::move (routing_id)] (
                               zlink::framework::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (
          zlink::framework::message_flow_log_mode_t::normal);
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (location_store);
        options.add_relocation_store (relocation_store);
        options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
        auto mesh = options.add_route_mesh (host_relocation_mesh_name);
        mesh.channel_name ("host-relocation-channel").server ();
        mesh.set_object_role (zlink::framework::object_role_t::server)
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from (routing_id))
          .add_spot_factory<relocation_ready_spot_t> (
            "host-relocation-spot",
            [] (zlink::framework::spot_context_t context) {
                return std::make_shared<relocation_ready_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.set_execution_mode (
                  zlink::framework::user_spot_execution_mode_t::spot_wide);
                factory.set_relocation_coordination_mode (
                  zlink::framework::spot_relocation_coordination_mode_t::application_signaled);
                factory.template preserve_state_with<relocation_ready_adapter_t> ();
            });
    });
}

bool verify_relocating_status_closes_admission ()
{
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (source, location_store, relocation_store,
                              "host-status-relocation-source");
    auto source_service = std::make_unique<relocation_source_service_t> (source, submit_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));
    char source_program[] = "host-status-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until (
          [&] {
              return source.is_ready ()
                     && source_service_view->created_spot.load (std::memory_order_acquire);
          },
          std::chrono::seconds (7))) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "status source must create its application-signaled Spot\n";
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_relocation_app (target, location_store, relocation_store,
                              "host-status-relocation-target");
    char target_program[] = "host-status-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (2))) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        std::cerr << "status relocation target must reach Serving\n";
        return false;
    }

    auto source_services = source.advanced ().services ().build_provider ();
    auto target_services = target.advanced ().services ().build_provider ();
    auto &source_mesh = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    auto &target_mesh = target_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    const auto peers_ready = wait_until (
      [&] {
          return source_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0
                 && target_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0;
      },
      std::chrono::seconds (7));
    auto &runtime = source_services.get_required<zlink::framework::framework_runtime_t> ();
    auto relocation =
      source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = std::chrono::milliseconds (250)});
    zlink::framework::framework_runtime_status_t relocating_status;
    const auto observed_relocating = wait_until (
      [&] {
          relocating_status = runtime.status ();
          return relocating_status.state == zlink::framework::framework_runtime_state_t::relocating;
      },
      std::chrono::milliseconds (100));
    const auto result = relocation.result ().value ();
    const auto serving_status = runtime.status ();
    source_services.close ();
    target_services.close ();

    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    const bool passed =
      peers_ready && observed_relocating && !relocating_status.is_ready
      && !relocating_status.accepting_work
      && result.outcome == zlink::framework::relocation_outcome_t::blocked
      && result.reason == zlink::framework::relocation_reason_t::deadline_exceeded
      && serving_status.state == zlink::framework::framework_runtime_state_t::serving
      && serving_status.is_ready && serving_status.accepting_work
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_exit_code == 0 && source_exit_code == 0;
    if (!passed) {
        std::cerr << "Relocating status must close admission and restore it only in Serving: "
                  << "peers=" << peers_ready << " observed=" << observed_relocating
                  << " accepting=" << relocating_status.accepting_work
                  << " outcome=" << static_cast<int> (result.outcome)
                  << " reason=" << static_cast<int> (result.reason)
                  << " serving-state=" << static_cast<int> (serving_status.state)
                  << " serving-ready=" << serving_status.is_ready
                  << " serving-accepting=" << serving_status.accepting_work
                  << " target-stop=" << static_cast<int> (target_stopped.outcome) << ":"
                  << static_cast<int> (target_stopped.reason)
                  << " source-stop=" << static_cast<int> (source_stopped.outcome) << ":"
                  << static_cast<int> (source_stopped.reason) << " target-exit=" << target_exit_code
                  << " source-exit=" << source_exit_code << '\n';
    }
    return passed;
}

bool verify_relocation_join_ignores_deadline ()
{
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (source, location_store, relocation_store,
                              "host-join-relocation-source");
    auto source_service = std::make_unique<relocation_source_service_t> (source, submit_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));
    char source_program[] = "host-join-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until (
          [&] {
              return source.is_ready ()
                     && source_service_view->created_spot.load (std::memory_order_acquire);
          },
          std::chrono::seconds (7))) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "join source must create its application-signaled Spot\n";
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_relocation_app (target, location_store, relocation_store,
                              "host-join-relocation-target");
    char target_program[] = "host-join-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (2))) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        std::cerr << "join relocation target must reach Serving\n";
        return false;
    }

    auto source_services = source.advanced ().services ().build_provider ();
    auto target_services = target.advanced ().services ().build_provider ();
    auto &source_mesh = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    auto &target_mesh = target_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    const auto peers_ready = wait_until (
      [&] {
          return source_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0
                 && target_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0;
      },
      std::chrono::seconds (7));
    auto &runtime = source_services.get_required<zlink::framework::framework_runtime_t> ();

    // First call starts the shared operation and holds it in-flight: the
    // application-signaled readiness is never submitted, so the worker waits
    // until the first call's deadline. The relocation terminal itself is not
    // under test here; the join contract is.
    auto first = source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                                   .deadline = std::chrono::milliseconds (2000)});
    const auto observed_relocating = wait_until (
      [&] {
          return runtime.status ().state == zlink::framework::framework_runtime_state_t::relocating;
      },
      std::chrono::seconds (1));
    // Second concurrent call: same mode and effective target application
    // version, different deadline. Per the frozen host configuration contract
    // it must join the running operation and receive the same terminal result;
    // its deadline must neither extend nor shorten the shared operation.
    auto second =
      source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = std::chrono::milliseconds (4000)});
    const auto first_result = first.result ().value ();
    const auto second_result = second.result ().value ();

    source_services.close ();
    target_services.close ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    const bool joined_same_terminal = second_result == first_result;
    const bool not_rejected =
      second_result.reason != zlink::framework::relocation_reason_t::operation_in_progress;
    const bool passed =
      peers_ready && observed_relocating && joined_same_terminal && not_rejected
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_exit_code == 0 && source_exit_code == 0;
    if (!passed) {
        std::cerr << "Concurrent relocation with a different deadline must join the "
                     "running operation and share its terminal: "
                  << "peers=" << peers_ready << " observed=" << observed_relocating
                  << " first(outcome=" << static_cast<int> (first_result.outcome)
                  << " reason=" << static_cast<int> (first_result.reason) << ")"
                  << " second(outcome=" << static_cast<int> (second_result.outcome)
                  << " reason=" << static_cast<int> (second_result.reason) << ")"
                  << " same=" << joined_same_terminal << " not_rejected=" << not_rejected << '\n';
    }
    return passed;
}

bool verify_relocation_reject_reports_requested_target ()
{
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (source, location_store, relocation_store,
                              "host-reject-relocation-source");
    auto source_service = std::make_unique<relocation_source_service_t> (source, submit_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));
    char source_program[] = "host-reject-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until (
          [&] {
              return source.is_ready ()
                     && source_service_view->created_spot.load (std::memory_order_acquire);
          },
          std::chrono::seconds (7))) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "reject source must create its application-signaled Spot\n";
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_relocation_app (target, location_store, relocation_store,
                              "host-reject-relocation-target");
    char target_program[] = "host-reject-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (2))) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        std::cerr << "reject relocation target must reach Serving\n";
        return false;
    }

    auto source_services = source.advanced ().services ().build_provider ();
    auto target_services = target.advanced ().services ().build_provider ();
    auto &source_mesh = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    auto &target_mesh = target_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    const auto peers_ready = wait_until (
      [&] {
          return source_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0
                 && target_mesh.snapshot (host_relocation_mesh_name).ready_peer_count > 0;
      },
      std::chrono::seconds (7));
    auto &runtime = source_services.get_required<zlink::framework::framework_runtime_t> ();

    // A planned_maintenance operation is started and held in-flight.
    auto first = source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                                   .deadline = std::chrono::milliseconds (2000)});
    const auto observed_relocating = wait_until (
      [&] {
          return runtime.status ().state == zlink::framework::framework_runtime_state_t::relocating;
      },
      std::chrono::seconds (1));
    // A concurrent call with a different mode and effective target application
    // version is rejected with blocked/operation_in_progress. Its result's mode
    // and effective target version must reflect the valid option the rejected
    // call requested (here rolling_update to version 7), not the running
    // operation's target.
    const std::int64_t requested_target = 7;
    const auto rejected =
      source.relocate ({.mode = zlink::framework::relocation_mode_t::rolling_update,
                        .target_application_version = requested_target,
                        .deadline = std::chrono::milliseconds (4000)});
    const auto rejected_result = rejected.result ().value ();
    const auto first_result = first.result ().value ();

    source_services.close ();
    target_services.close ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    const bool passed =
      peers_ready && observed_relocating
      && rejected_result.outcome == zlink::framework::relocation_outcome_t::blocked
      && rejected_result.reason == zlink::framework::relocation_reason_t::operation_in_progress
      && rejected_result.mode == zlink::framework::relocation_mode_t::rolling_update
      && rejected_result.effective_target_application_version == requested_target
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_exit_code == 0 && source_exit_code == 0;
    if (!passed) {
        std::cerr << "Rejected concurrent relocation must report the requested option: " << "peers="
                  << peers_ready << " observed=" << observed_relocating
                  << " rejected(outcome=" << static_cast<int> (rejected_result.outcome)
                  << " reason=" << static_cast<int> (rejected_result.reason)
                  << " mode=" << static_cast<int> (rejected_result.mode)
                  << " effective=" << rejected_result.effective_target_application_version
                  << ") expected effective=" << requested_target
                  << " first(reason=" << static_cast<int> (first_result.reason) << ")\n";
    }
    return passed;
}

bool verify_application_signaled_relocation (bool shutdown_during_move)
{
    constexpr auto ready_timeout = std::chrono::seconds (7);
    constexpr auto shutdown_timeout = std::chrono::seconds (5);
    relocation_ready_spot_t::readiness_entered =
      std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    relocation_ready_spot_t::readiness_release =
      std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    relocation_ready_spot_t::completion_count.store (0, std::memory_order_release);
    relocation_ready_spot_t::last_outcome.store (-1, std::memory_order_release);
    relocation_ready_adapter_t::restored.store (false, std::memory_order_release);
    if (shutdown_during_move) {
        relocation_ready_adapter_t::capture_entered =
          std::make_shared<zlink::framework::task_completion_source_t<void>> ();
        relocation_ready_adapter_t::capture_release =
          std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    }

    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (source, location_store, relocation_store,
                              "zz-host-relocation-source");
    if (shutdown_during_move) {
        source.set_message_flow_mode (zlink::framework::message_flow_log_mode_t::normal);
        source.logging ()
          .use_file (
            (std::filesystem::temp_directory_path () / "accepted-source-shutdown-source.flow")
              .string ())
          .set_min_level (zlink::framework::log_level_t::debug);
    }
    auto source_service = std::make_unique<relocation_source_service_t> (source, submit_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));

    char source_program[] = "host-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until (
          [&] {
              return source.is_ready ()
                     && source_service_view->created_spot.load (std::memory_order_acquire);
          },
          ready_timeout)) {
        std::cerr << "source app must create the application-signaled Spot: ready="
                  << source.is_ready () << " created=" << source_service_view->created_spot.load ()
                  << " error=" << source_service_view->error << '\n';
        const auto scanned =
          location_store->scan ({.prefix = "zlink:v11:", .limit = 100}).result ().value ();
        if (const auto *page = std::get_if<zlink::framework::store_scan_page_t> (&scanned)) {
            for (const auto &item : page->items) {
                std::string value;
                value.reserve (item.value.bytes.size ());
                for (const auto byte : item.value.bytes)
                    value.push_back (static_cast<char> (std::to_integer<unsigned char> (byte)));
                std::cerr << "location-key=" << item.key.value << " value=" << value << '\n';
            }
        }
        source.request_stop ();
        source_thread.join ();
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_relocation_app (target, location_store, relocation_store,
                              "aa-host-relocation-target");
    if (shutdown_during_move) {
        target.set_message_flow_mode (zlink::framework::message_flow_log_mode_t::normal);
        target.logging ()
          .use_file (
            (std::filesystem::temp_directory_path () / "accepted-source-shutdown-target.flow")
              .string ())
          .set_min_level (zlink::framework::log_level_t::debug);
    }
    char target_program[] = "host-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] {
        /* Relocate starts before this replacement publishes its descriptor.
         * The source must keep checking Store and Core peer readiness until
         * the shared deadline instead of rejecting the first empty snapshot. */
        if (!shutdown_during_move)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        target_exit_code = target.run (1, target_arguments);
    });

    submit_ready->store (true, std::memory_order_release);
    const bool readiness_accepted =
      wait_until ([] { return relocation_ready_spot_t::readiness_entered->task ().await_ready (); },
                  ready_timeout);
    auto relocation =
      source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = std::chrono::seconds (10)});
    const bool relocating =
      wait_until (
        [&] {
            return source.runtime_state ()
                     == zlink::framework::framework_runtime_state_t::relocating
                   || relocation.await_ready ();
        },
        ready_timeout)
      && source.runtime_state () == zlink::framework::framework_runtime_state_t::relocating;
    relocation_ready_spot_t::readiness_release->complete (
      zlink::framework::result_t<void>::success ());
    if (shutdown_during_move) {
        // The source has accepted and sealed this unit before capture enters.
        const bool accepted = wait_until (
          [] { return relocation_ready_adapter_t::capture_entered->task ().await_ready (); },
          ready_timeout);
        auto stopping = source.shutdown (shutdown_timeout);
        const bool held = !stopping.await_ready ();
        relocation_ready_adapter_t::capture_release->complete (
          zlink::framework::result_t<void>::success ());
        const auto moved = relocation.result ().value ();
        const auto stopped = stopping.result ().value ();
        const auto target_stopped = target.shutdown (shutdown_timeout).result ().value ();
        source_thread.join ();
        target_thread.join ();
        relocation_ready_spot_t::readiness_entered.reset ();
        relocation_ready_spot_t::readiness_release.reset ();
        relocation_ready_adapter_t::capture_entered.reset ();
        relocation_ready_adapter_t::capture_release.reset ();
        const bool passed =
          readiness_accepted && relocating && accepted && held
          && moved.outcome == zlink::framework::relocation_outcome_t::blocked
          && moved.reason == zlink::framework::relocation_reason_t::shutdown_requested
          && stopped.outcome == zlink::framework::termination_outcome_t::stopped
          && stopped.reason == zlink::framework::termination_reason_t::none
          && source.runtime_state () == zlink::framework::framework_runtime_state_t::stopped
          && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
          && source_exit_code == 0 && target_exit_code == 0;
        std::cout << "accepted-source-shutdown readiness-accepted=" << readiness_accepted
                  << " relocating=" << relocating << " accepted=" << accepted << " held=" << held
                  << " move=" << static_cast<int> (moved.outcome)
                  << " reason=" << static_cast<int> (moved.reason)
                  << " stopped=" << static_cast<int> (stopped.outcome)
                  << " termination-reason=" << static_cast<int> (stopped.reason)
                  << " ready-sent=" << source_service_view->ready_sent.load ()
                  << " service-error=" << source_service_view->error << " passed=" << passed
                  << '\n';
        return passed;
    }
    const auto result = relocation.result ().value ();

    const auto target_stopped = target.shutdown (shutdown_timeout).result ().value ();
    const auto source_stopped = source.shutdown (shutdown_timeout).result ().value ();
    target_thread.join ();
    source_thread.join ();

    relocation_ready_spot_t::readiness_entered.reset ();
    relocation_ready_spot_t::readiness_release.reset ();
    const bool passed =
      readiness_accepted && relocating
      && source_service_view->ready_sent.load (std::memory_order_acquire)
      && source_service_view->error.empty ()
      && result.outcome == zlink::framework::relocation_outcome_t::relocated
      && relocation_ready_adapter_t::restored.load (std::memory_order_acquire)
      && relocation_ready_spot_t::completion_count.load (std::memory_order_acquire) == 1
      && relocation_ready_spot_t::last_outcome.load (std::memory_order_acquire)
           == static_cast<int> (zlink::framework::spot_relocation_ready_outcome_t::relocated)
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_exit_code == 0 && source_exit_code == 0;
    if (!passed) {
        std::cerr << "public app relocation diagnostic: outcome="
                  << static_cast<int> (result.outcome)
                  << " reason=" << static_cast<int> (result.reason)
                  << " ready=" << source_service_view->ready_sent.load ()
                  << " restored=" << relocation_ready_adapter_t::restored.load ()
                  << " completions=" << relocation_ready_spot_t::completion_count.load ()
                  << " completion-outcome=" << relocation_ready_spot_t::last_outcome.load ()
                  << " service-error=" << source_service_view->error << '\n';
    }
    return passed;
}
}
