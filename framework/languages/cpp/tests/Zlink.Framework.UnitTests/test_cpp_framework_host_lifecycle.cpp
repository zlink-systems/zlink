/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include "runtime/locations/in_memory_store_providers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

struct relocation_ready_message_t
{
    int value = 0;
};

class relocation_ready_spot_t final
    : public zlink::framework::spot_t<
        zlink::framework::actor_t>
{
  public:
    explicit relocation_ready_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::spot_context_t &
    context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<
          &relocation_ready_spot_t::on_ready> (
            "relocation-ready");
    }

    zlink::framework::task_t<void> on_ready (
      const relocation_ready_message_t &)
    {
        _context.relocation_ready ().defer ();
        co_return;
    }

    zlink::framework::task_t<
      zlink::framework::spot_create_response_t>
    on_create (
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::
          accept ();
    }

    zlink::framework::task_t<void>
    on_initialize () override
    {
        co_return;
    }

    zlink::framework::task_t<
      zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::
          spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (
      zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (
      zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_relocation_ready_completed (
      const zlink::framework::
        spot_relocation_ready_completion_t &completion) override
    {
        last_outcome.store (
          static_cast<int> (completion.outcome),
          std::memory_order_release);
        completion_count.fetch_add (
          1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::atomic_int completion_count{0};
    static inline std::atomic_int last_outcome{-1};

  private:
    zlink::framework::spot_context_t _context;
};

class relocation_ready_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<
        relocation_ready_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (
      relocation_ready_spot_t &,
      std::stop_token) override
    {
        co_return std::vector<std::byte>{
          std::byte{0x51}, std::byte{0x52}};
    }

    zlink::framework::task_t<void>
    restore (
      relocation_ready_spot_t &,
      std::vector<std::byte> payload,
      std::stop_token) override
    {
        restored.store (
          payload
            == std::vector<std::byte>{
              std::byte{0x51}, std::byte{0x52}},
          std::memory_order_release);
        co_return;
    }

    static inline std::atomic_bool restored{false};
};

class relocation_source_service_t final
    : public zlink::framework::hosted_service_t
{
  public:
    relocation_source_service_t (
      zlink::framework::app_t &app,
      std::shared_ptr<std::atomic_bool> submit_ready) :
        _app (&app), _submit_ready (std::move (submit_ready))
    {
    }

    void start (
      zlink::framework::service_provider_t &services) override
    {
        _runtime = &services.get_required<
          zlink::framework::route_mesh_runtime_t> ();
        auto manager = services.get_required<
          zlink::framework::spot_manager_t> ();
        auto client =
          _app->advanced ().zlink ().route_client (
            services.get_required<
              zlink::framework::serializer_registry_t> ());
        _sender = std::thread (
          [this, manager = std::move (manager),
           client = std::move (client)] () mutable {
              const auto create_deadline =
                std::chrono::steady_clock::now ()
                + std::chrono::seconds (5);
              std::string last_create_error;
              while (!_stop.load (std::memory_order_acquire)
                     && std::chrono::steady_clock::now ()
                          < create_deadline) {
                  const auto created =
                    manager.get_or_create (
                      zlink::framework::spot_id_t (
                        "host-relocation-spot"),
                      "host-relocation-spot")
                      .timeout (std::chrono::seconds (1))
                      .submit ().result ();
                  if (created) {
                      created_spot.store (
                        true, std::memory_order_release);
                      break;
                  }
                  if (last_create_error.empty ())
                      last_create_error =
                        created.error ()
                          ? created.error ()->what ()
                          : "Spot create failed";
                  std::this_thread::sleep_for (
                    std::chrono::milliseconds (10));
              }
              if (!created_spot.load (
                    std::memory_order_acquire)) {
                  error = std::move (last_create_error);
                  return;
              }
              while (!_stop.load (std::memory_order_acquire)
                     && !_submit_ready->load (
                       std::memory_order_acquire))
                  std::this_thread::sleep_for (
                    std::chrono::milliseconds (1));
              if (_stop.load (std::memory_order_acquire))
                  return;
              const auto submitted =
                client.send_to_spot (
                  zlink::framework::spot_id_t (
                    "host-relocation-spot"),
                  relocation_ready_message_t{1})
                  .submit ().result ();
              if (submitted)
                  ready_sent.store (
                    true, std::memory_order_release);
              else
                  error =
                    "readiness message submission failed";
          });
    }

    void stop () noexcept override
    {
        _stop.store (true, std::memory_order_release);
        _submit_ready->store (true, std::memory_order_release);
        if (_sender.joinable ())
            _sender.join ();
    }

    bool target_ready () const
    {
        if (!_runtime)
            return false;
        const auto snapshot =
          _runtime->snapshot ("host-relocation-mesh");
        return std::any_of (
          snapshot.peers.begin (), snapshot.peers.end (),
          [] (const auto &peer) {
              return peer.state == zlink::framework::peer_state_t::ready
                     && peer.node_rid
                          == zlink::routing_id_t::from (
                            "aa-host-relocation-target");
          });
    }

    zlink::framework::mesh_node_snapshot_t snapshot () const
    {
        return _runtime->snapshot ("host-relocation-mesh");
    }

    std::atomic_bool created_spot{false};
    std::atomic_bool ready_sent{false};
    std::string error;

  private:
    zlink::framework::app_t *_app;
    zlink::framework::route_mesh_runtime_t *_runtime = nullptr;
    std::shared_ptr<std::atomic_bool> _submit_ready;
    std::atomic_bool _stop{false};
    std::thread _sender;
};

bool wait_until (
  const std::function<bool ()> &condition,
  std::chrono::milliseconds timeout)
{
    const auto deadline =
      std::chrono::steady_clock::now () + timeout;
    while (!condition ()
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (
          std::chrono::milliseconds (1));
    return condition ();
}

void configure_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<
    zlink::framework::runtime::in_memory_location_store_t>
    &location_store,
  const std::shared_ptr<
    zlink::framework::runtime::in_memory_relocation_store_t>
    &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework (
      [location_store, relocation_store,
       routing_id = std::move (routing_id)] (
        zlink::framework::zlink_framework_options_t &options) {
          options.add_location_store (location_store);
          options.add_relocation_store (relocation_store);
          options.configure_locations ().polling_interval =
            std::chrono::milliseconds (10);
          auto mesh =
            options.add_route_mesh ("host-relocation-mesh");
          mesh.channel_name (
            "host-relocation-channel").server ();
          mesh
            .listen ("tcp://127.0.0.1:0")
            .set_routing_id (
              zlink::routing_id_t::from (routing_id))
            .add_spot_factory<relocation_ready_spot_t> (
              "host-relocation-spot",
              [] (zlink::framework::spot_context_t context) {
                  return std::make_shared<
                    relocation_ready_spot_t> (
                    std::move (context));
              },
              [] (auto &factory) {
                  factory.set_execution_mode (
                    zlink::framework::
                      user_spot_execution_mode_t::spot_wide);
                  factory.set_relocation_readiness (
                    zlink::framework::
                      spot_relocation_readiness_mode_t::
                        application_signaled);
                  factory.template preserve_state_with<
                    relocation_ready_adapter_t> ();
              });
      });
}

class blocking_stop_service_t final :
    public zlink::framework::hosted_service_t
{
  public:
    void start (zlink::framework::service_provider_t &) override
    {
        {
            std::lock_guard lock (_mutex);
            _started = true;
        }
        _changed.notify_all ();
    }

    void stop () noexcept override
    {
        std::unique_lock lock (_mutex);
        _stop_entered = true;
        _changed.notify_all ();
        _changed.wait (lock, [&] { return _release; });
    }

    void wait_started ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] { return _started; });
    }

    void wait_stop_entered ()
    {
        std::unique_lock lock (_mutex);
        _changed.wait (lock, [&] { return _stop_entered; });
    }

    void release ()
    {
        {
            std::lock_guard lock (_mutex);
            _release = true;
        }
        _changed.notify_all ();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _started = false;
    bool _stop_entered = false;
    bool _release = false;
};

bool verify_relocation_blocker (
  std::string_view label,
  std::function<void (zlink::framework::zlink_framework_options_t &)> configure,
  zlink::framework::relocation_reason_t expected)
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework (std::move (configure));
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    app.add_hosted_service (std::move (service));

    char program[] = "termination-topology-preflight";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
    service_view->wait_started ();
    const auto serving_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!app.is_ready ()
           && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto result =
      app.relocate (
           {.mode =
              zlink::framework::relocation_mode_t::planned_maintenance,
            .deadline = std::chrono::seconds (1)})
        .result ()
        .value ();
    const bool matched =
      result.outcome == zlink::framework::relocation_outcome_t::blocked
      && result.reason == expected
      && app.is_ready ();
    if (!matched)
        std::cerr << label
                  << " must block Relocate without changing Serving\n";

    auto shutdown = app.shutdown (std::chrono::seconds (2));
    service_view->wait_stop_entered ();
    service_view->release ();
    const auto stopped = shutdown.result ().value ();
    run_thread.join ();
    return matched
           && stopped.outcome
                == zlink::framework::termination_outcome_t::stopped
           && exit_code == 0;
}

bool verify_application_signaled_relocation ()
{
    relocation_ready_spot_t::completion_count.store (
      0, std::memory_order_release);
    relocation_ready_spot_t::last_outcome.store (
      -1, std::memory_order_release);
    relocation_ready_adapter_t::restored.store (
      false, std::memory_order_release);

    auto location_store = std::make_shared<
      zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store = std::make_shared<
      zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready =
      std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (
      source, location_store, relocation_store,
      "zz-host-relocation-source");
    auto source_service =
      std::make_unique<relocation_source_service_t> (
        source, submit_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));

    char source_program[] = "host-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] {
        source_exit_code =
          source.run (1, source_arguments);
    });
    if (!wait_until (
          [&] {
              return source.is_ready ()
                     && source_service_view
                          ->created_spot.load (
                            std::memory_order_acquire);
          },
          std::chrono::seconds (7))) {
        std::cerr
          << "source app must create the application-signaled Spot: ready="
          << source.is_ready ()
          << " created="
          << source_service_view->created_spot.load ()
          << " error=" << source_service_view->error
          << '\n';
        const auto scanned =
          location_store->scan (
            {.prefix = "zlink:v11:", .limit = 100})
            .result ().value ();
        if (const auto *page = std::get_if<
              zlink::framework::store_scan_page_t> (
              &scanned)) {
            for (const auto &item : page->items)
            {
                std::string value;
                value.reserve (item.value.bytes.size ());
                for (const auto byte : item.value.bytes)
                    value.push_back (
                      static_cast<char> (
                        std::to_integer<unsigned char> (
                          byte)));
                std::cerr << "location-key="
                          << item.key.value
                          << " value=" << value << '\n';
            }
        }
        source.request_stop ();
        source_thread.join ();
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_relocation_app (
      target, location_store, relocation_store,
      "aa-host-relocation-target");
    char target_program[] = "host-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] {
        target_exit_code =
          target.run (1, target_arguments);
    });
    if (!wait_until (
          [&] { return target.is_ready (); },
          std::chrono::seconds (5))) {
        std::cerr << "target app must reach Serving\n";
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        return false;
    }
    if (!wait_until (
          [&] {
              return source_service_view->target_ready ();
          },
          std::chrono::seconds (5))) {
        std::cerr
          << "source app must admit the target peer before relocation";
        const auto snapshot = source_service_view->snapshot ();
        for (const auto &peer : snapshot.peers)
            std::cerr << " peer=" << peer.node_rid.to_string ()
                      << " state=" << static_cast<int> (peer.state)
                      << " unavailable="
                      << peer.unavailable_reason.has_value ();
        std::cerr << '\n';
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        return false;
    }

    auto relocation = source.relocate (
      {.mode =
         zlink::framework::relocation_mode_t::
           planned_maintenance,
       .deadline = std::chrono::seconds (10)});
    submit_ready->store (true, std::memory_order_release);
    const auto result = relocation.result ().value ();

    const auto target_stopped =
      target.shutdown (std::chrono::seconds (5))
        .result ().value ();
    const auto source_stopped =
      source.shutdown (std::chrono::seconds (5))
        .result ().value ();
    target_thread.join ();
    source_thread.join ();

    const bool passed =
      source_service_view->ready_sent.load (
        std::memory_order_acquire)
      && source_service_view->error.empty ()
      && result.outcome
           == zlink::framework::relocation_outcome_t::relocated
      && relocation_ready_adapter_t::restored.load (
        std::memory_order_acquire)
      && relocation_ready_spot_t::completion_count.load (
           std::memory_order_acquire)
           == 1
      && relocation_ready_spot_t::last_outcome.load (
           std::memory_order_acquire)
           == static_cast<int> (
             zlink::framework::
               spot_relocation_ready_outcome_t::relocated)
      && target_stopped.outcome
           == zlink::framework::termination_outcome_t::stopped
      && source_stopped.outcome
           == zlink::framework::termination_outcome_t::stopped
      && target_exit_code == 0
      && source_exit_code == 0;
    if (!passed) {
        std::cerr
          << "public app relocation diagnostic: outcome="
          << static_cast<int> (result.outcome)
          << " reason=" << static_cast<int> (result.reason)
          << " ready="
          << source_service_view->ready_sent.load ()
          << " restored="
          << relocation_ready_adapter_t::restored.load ()
          << " completions="
          << relocation_ready_spot_t::completion_count.load ()
          << " completion-outcome="
          << relocation_ready_spot_t::last_outcome.load ()
          << " service-error=" << source_service_view->error
          << '\n';
    }
    return passed;
}

} // namespace

int main ()
{
    if (std::getenv (
          "ZLINK_CPP_RUN_HOST_RELOCATION_INTEGRATION")
        && !verify_application_signaled_relocation ())
        return EXIT_FAILURE;

    if (!verify_relocation_blocker (
          "manual ClientServer topology",
          [] (zlink::framework::zlink_framework_options_t &options) {
              options.add_client_server_channel ("manual-orders")
                .client ()
                .connect ("tcp://127.0.0.1:29999");
          },
          zlink::framework::relocation_reason_t::manual_topology_unsupported)) {
        return EXIT_FAILURE;
    }
    if (!verify_relocation_blocker (
          "manual RouteMesh topology",
          [] (zlink::framework::zlink_framework_options_t &options) {
              auto node = options.add_route_mesh ("retire-manual-mesh");
              node.channel_name ("retire-manual-channel").client ();
              node.set_routing_id (
                    zlink::routing_id_t::from ("retire-manual-node"))
                .listen ("inproc://cpp-retire-manual-node");
              node.peer_connections ().connect (
                "tcp://127.0.0.1:29998");
          },
          zlink::framework::relocation_reason_t::manual_topology_unsupported)) {
        return EXIT_FAILURE;
    }
    if (!verify_relocation_blocker (
          "automatic RouteMesh without a replacement",
          [] (zlink::framework::zlink_framework_options_t &options) {
              auto node = options.add_route_mesh ("retire-single-mesh");
              node.channel_name ("retire-single-channel").client ();
              node.set_routing_id (
                    zlink::routing_id_t::from ("retire-single-node"))
                .listen ("inproc://cpp-retire-single-node");
          },
          zlink::framework::relocation_reason_t::target_unavailable)) {
        return EXIT_FAILURE;
    }

    auto app = zlink::framework::app_t::create ();
    if (app.runtime_state ()
        != zlink::framework::framework_runtime_state_t::preparing) {
        std::cerr << "new app must begin in Preparing\n";
        return EXIT_FAILURE;
    }

    bool planned_target_rejected = false;
    try {
        (void) app.relocate (
          {.mode =
             zlink::framework::relocation_mode_t::planned_maintenance,
           .target_application_version = 2});
    }
    catch (const std::invalid_argument &) {
        planned_target_rejected = true;
    }
    bool rolling_target_required = false;
    try {
        (void) app.relocate (
          {.mode = zlink::framework::relocation_mode_t::rolling_update});
    }
    catch (const std::invalid_argument &) {
        rolling_target_required = true;
    }
    if (!planned_target_rejected || !rolling_target_required) {
        std::cerr << "Relocate mode must validate its target version option\n";
        return EXIT_FAILURE;
    }

    const auto relocation =
      app.relocate (
           {.mode =
              zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (relocation.outcome
          != zlink::framework::relocation_outcome_t::blocked
        || relocation.reason
             != zlink::framework::relocation_reason_t::runtime_not_ready) {
        std::cerr
          << "Relocate before Serving must return Blocked/RuntimeNotReady\n";
        return EXIT_FAILURE;
    }

    const auto shutdown =
      app.shutdown (std::chrono::seconds (1)).result ().value ();
    if (shutdown.outcome
             != zlink::framework::termination_outcome_t::stopped
        || shutdown.reason
             != zlink::framework::termination_reason_t::none
        || app.runtime_state ()
             != zlink::framework::framework_runtime_state_t::stopped) {
        std::cerr << "Shutdown must complete the shared termination operation\n";
        return EXIT_FAILURE;
    }
    const auto after_shutdown =
      app.relocate (
           {.mode =
              zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (after_shutdown.outcome
          != zlink::framework::relocation_outcome_t::blocked
        || after_shutdown.reason
             != zlink::framework::relocation_reason_t::runtime_not_ready) {
        std::cerr << "Relocate after Shutdown must report RuntimeNotReady\n";
        return EXIT_FAILURE;
    }

    auto running = zlink::framework::app_t::create ();
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    running.add_hosted_service (std::move (service));
    char program[] = "termination-facade";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread (
      [&] { exit_code = running.run (1, arguments); });
    service_view->wait_started ();
    const auto serving_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!running.is_ready ()
           && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto unavailable_relocation =
      running
        .relocate (
          {.mode =
             zlink::framework::relocation_mode_t::planned_maintenance,
           .deadline = std::chrono::seconds (2)})
        .result ()
        .value ();
    if (unavailable_relocation.outcome
          != zlink::framework::relocation_outcome_t::blocked
        || unavailable_relocation.reason
             != zlink::framework::relocation_reason_t::target_unavailable
        || !running.is_ready ()) {
        std::cerr << "Relocation preflight blocker must preserve Serving\n";
        running.stop ();
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    auto shared_shutdown = running.shutdown (std::chrono::seconds (2));
    service_view->wait_stop_entered ();
    if (shared_shutdown.await_ready ()) {
        std::cerr << "Shutdown must wait for hosted-service teardown\n";
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    std::stop_source cancelled_source;
    cancelled_source.request_stop ();
    auto cancelled_waiter =
      running.shutdown (
        std::chrono::seconds (2), cancelled_source.get_token ());
    const auto &cancelled = cancelled_waiter.result ();
    if (cancelled
        || !cancelled.error ()
        || cancelled.error ()->code ()
             != std::make_error_code (std::errc::operation_canceled)) {
        std::cerr << "wait cancellation must cancel only the joining waiter\n";
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    service_view->release ();
    const auto shared_result = shared_shutdown.result ().value ();
    run_thread.join ();
    if (shared_result.outcome
          != zlink::framework::termination_outcome_t::stopped
        || exit_code != 0) {
        std::cerr << "shared Shutdown must survive waiter cancellation\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
