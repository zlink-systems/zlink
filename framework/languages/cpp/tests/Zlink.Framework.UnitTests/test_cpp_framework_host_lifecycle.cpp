/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_repository.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/host/relocation_target_eligibility.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

struct degraded_channel_message_t
{
};

struct degraded_channel_handler_t
{
    using message_type = degraded_channel_message_t;

    void handle (const degraded_channel_message_t &) {}
};

struct degraded_object_message_t
{
    static constexpr const char *packet_name = "degraded-object-message";
    int value = 0;
};

void to_json (nlohmann::json &json, const degraded_object_message_t &message)
{
    json = nlohmann::json{{"value", message.value}};
}

void from_json (const nlohmann::json &json, degraded_object_message_t &message)
{
    message.value = json.at ("value").get<int> ();
}

class degraded_object_spot_t;

struct degraded_object_timer_handler_t
{
    zlink::framework::task_t<void> handle (degraded_object_spot_t &,
                                           const zlink::framework::timer_tick_t &)
    {
        timer_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::atomic_int timer_count{0};
};

class degraded_object_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit degraded_object_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&degraded_object_spot_t::on_message> ();
        (void) _context.add_timer<degraded_object_timer_handler_t> ("degraded-object-timer",
                                                                    std::chrono::milliseconds (5));
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override { co_return; }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    void on_message (const degraded_object_message_t &message)
    {
        message_value.store (message.value, std::memory_order_release);
    }

    static inline std::atomic_int factory_count{0};
    static inline std::atomic_int message_value{0};

  private:
    zlink::framework::spot_context_t _context;
};

struct relocation_ready_message_t
{
    int value = 0;
};

class relocation_ready_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit relocation_ready_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&relocation_ready_spot_t::on_ready> ("relocation-ready");
    }

    zlink::framework::task_t<void> on_ready (const relocation_ready_message_t &)
    {
        _context.relocation_ready ().defer ();
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override { co_return; }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_relocation_ready_completed (
      const zlink::framework::spot_relocation_ready_completion_t &completion) override
    {
        last_outcome.store (static_cast<int> (completion.outcome), std::memory_order_release);
        completion_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::atomic_int completion_count{0};
    static inline std::atomic_int last_outcome{-1};

  private:
    zlink::framework::spot_context_t _context;
};

class relocation_ready_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<relocation_ready_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>> capture (relocation_ready_spot_t &,
                                                              std::stop_token) override
    {
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
};

class configuration_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit configuration_actor_t (zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override { return _context; }

  private:
    zlink::framework::actor_context_t _context;
};

class configuration_actor_factory_t final
    : public zlink::framework::actor_factory_t<configuration_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<configuration_actor_t>>
    create (zlink::framework::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<configuration_actor_t> (std::move (context));
    }
};

class remote_create_entry_spot_t final
    : public zlink::framework::entry_spot_t<configuration_actor_t>
{
  public:
    explicit remote_create_entry_spot_t (zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::entry_spot_context_t &context () noexcept override { return _context; }

    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_send<&remote_create_entry_spot_t::on_probe> (
          "remote-create-probe");
    }

    zlink::framework::task_t<zlink::framework::actor_create_response_t>
    on_create_actor (configuration_actor_t &, const zlink::framework::message_t &) override
    {
        created_count.fetch_add (1, std::memory_order_acq_rel);
        co_return zlink::framework::actor_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_actor_joined (configuration_actor_t &) override
    {
        joined_count.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (configuration_actor_t &) override { co_return; }

    zlink::framework::task_t<void> on_disconnect_actor (configuration_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_probe (configuration_actor_t &,
                                             zlink::framework::message_context_t &,
                                             const relocation_ready_message_t &)
    {
        co_return;
    }

    static inline std::atomic_int created_count{0};
    static inline std::atomic_int joined_count{0};

  private:
    zlink::framework::entry_spot_context_t _context;
};

class configuration_instance_spot_t final : public zlink::framework::instance_spot_t
{
  public:
    explicit configuration_instance_spot_t (zlink::framework::instance_spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::instance_spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::instance_spot_context_t &context () const noexcept override
    {
        return _context;
    }
    void configure () override {}

  private:
    zlink::framework::instance_spot_context_t _context;
};

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
        _runtime = &services.get_required<zlink::framework::route_mesh_runtime_t> ();
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
                  error = "readiness message submission failed";
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
    zlink::framework::route_mesh_runtime_t *_runtime = nullptr;
    std::shared_ptr<std::atomic_bool> _submit_ready;
    std::atomic_bool _stop{false};
    std::thread _sender;
};

bool wait_until (const std::function<bool ()> &condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (!condition () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    return condition ();
}

std::vector<std::byte> bytes_from_hex (std::string_view value)
{
    const auto digit = [] (char character) -> unsigned char {
        return character <= '9' ? static_cast<unsigned char> (character - '0')
                                : static_cast<unsigned char> (character - 'a' + 10);
    };
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (
          static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

class observing_actor_creation_store_t final : public zlink::framework::location_store_t
{
  public:
    explicit observing_actor_creation_store_t (
      std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> inner) :
        inner (std::move (inner))
    {
    }

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        if (key.value.starts_with (std::string ("creation-terminal") + '\0')) {
            terminal_reads.fetch_add (1, std::memory_order_acq_rel);
            if (hide_terminal_reads.load (std::memory_order_acquire)) {
                return zlink::framework::task_t<zlink::framework::store_read_result_t> (
                  zlink::framework::result_t<zlink::framework::store_read_result_t>::success (
                    zlink::framework::store_missing_t{std::chrono::system_clock::now ()}));
            }
            if (terminal_override) {
                const auto now = std::chrono::system_clock::now ();
                return zlink::framework::task_t<zlink::framework::store_read_result_t> (
                  zlink::framework::result_t<zlink::framework::store_read_result_t>::success (
                    zlink::framework::store_found_t{{*terminal_override,
                                                     {"node-production-schema"},
                                                     now + std::chrono::minutes (1),
                                                     now}}));
            }
        }
        return inner->read (std::move (key));
    }

    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        bool writes_terminal = false;
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<zlink::framework::store_put_t> (&mutation);
                put && put->key.value.starts_with (std::string ("creation-terminal") + '\0')) {
                writes_terminal = true;
                std::lock_guard lock (terminal_mutex);
                published_terminal = put->bytes;
            }
        }
        auto written = inner->write (std::move (request));
        if (!writes_terminal || !force_terminal_write_conflict.load (std::memory_order_acquire))
            return written;
        written.result ().value ();
        return zlink::framework::task_t<zlink::framework::store_write_result_t> (
          zlink::framework::result_t<zlink::framework::store_write_result_t>::success (
            zlink::framework::store_write_conflict_t{std::chrono::system_clock::now ()}));
    }

    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return inner->scan (std::move (request));
    }

    std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> inner;
    std::atomic_size_t terminal_reads{0};
    std::optional<std::vector<std::byte>> terminal_override;
    std::atomic_bool hide_terminal_reads{false};
    std::atomic_bool force_terminal_write_conflict{false};
    std::mutex terminal_mutex;
    std::vector<std::byte> published_terminal;
};

void configure_remote_actor_create_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<observing_actor_creation_store_t> &location_store,
  std::string routing_id,
  bool actor_target)
{
    app.add_zlink_framework ([location_store, routing_id = std::move (routing_id),
                              actor_target] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (location_store);
        options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
        auto mesh = options.add_route_mesh ("host-remote-actor-create-mesh");
        mesh.set_object_role (zlink::framework::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (routing_id))
          .listen ("tcp://127.0.0.1:0");
        if (actor_target) {
            mesh
              .add_entry_spot<remote_create_entry_spot_t> (
                [] (zlink::framework::entry_spot_context_t context) {
                    return std::make_shared<remote_create_entry_spot_t> (std::move (context));
                })
              .add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                "remote-create-actor", std::make_shared<configuration_actor_factory_t> (),
                [] (auto &factory) { factory.disable_relocation (); });
        }
    });
}

bool verify_remote_actor_create_target_owns_completion ()
{
    remote_create_entry_spot_t::created_count.store (0, std::memory_order_release);
    remote_create_entry_spot_t::joined_count.store (0, std::memory_order_release);
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto target_location_store =
      std::make_shared<observing_actor_creation_store_t> (location_store);
    auto source_location_store =
      std::make_shared<observing_actor_creation_store_t> (location_store);

    auto target = zlink::framework::app_t::create ();
    configure_remote_actor_create_app (target, target_location_store, "host-remote-create-target",
                                       true);
    char target_program[] = "host-remote-create-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (3))) {
        target.request_stop ();
        target_thread.join ();
        std::cerr << "remote Actor create target must reach Serving\n";
        return false;
    }

    auto source = zlink::framework::app_t::create ();
    configure_remote_actor_create_app (source, source_location_store, "host-remote-create-source",
                                       false);
    char source_program[] = "host-remote-create-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until ([&] { return source.is_ready (); }, std::chrono::seconds (3))) {
        source.request_stop ();
        target.request_stop ();
        source_thread.join ();
        target_thread.join ();
        std::cerr << "remote Actor create source must reach Serving\n";
        return false;
    }

    auto source_services = source.advanced ().services ().build_provider ();
    auto &routes = source_services.get_required<zlink::framework::route_mesh_runtime_t> ();
    const auto route_ready = wait_until (
      [&] { return routes.snapshot ("host-remote-actor-create-mesh").ready_peer_count == 1; },
      std::chrono::seconds (5));

    auto &actors = source_services.get_required<zlink::framework::actor_manager_t> ();
    const auto created =
      actors.get_or_create (zlink::framework::actor_id_t ("actor-canonical"), "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    auto &location_repository =
      source_services.get_required<zlink::framework::location_repository_t> ();
    const auto authority =
      location_repository
        .read_authority (zlink::framework::runtime::actor_authority_key ("actor-canonical"))
        .result ()
        .value ();
    const auto *authority_snapshot =
      std::get_if<zlink::framework::authority_snapshot_t> (&authority);
    const bool authority_active =
      authority_snapshot
      && authority_snapshot->allocation.state
           == zlink::framework::placement_allocation_state_t::active
      && authority_snapshot->allocation.target.node_rid.value () == "host-remote-create-target"
      && !authority_snapshot->pending_creation;
    // The requester probes once before reserving; another terminal read would
    // mean it tried to complete the target-owned reservation after the reply.
    const auto source_terminal_reads =
      source_location_store->terminal_reads.load (std::memory_order_acquire);
    const auto node_terminal =
      bytes_from_hex ("01000000250000000000000000010200180f6163746f722d63616e6f6e6963616c"
                      "000000000000000100");
    bool cpp_schema_encoded = false;
    {
        std::lock_guard lock (target_location_store->terminal_mutex);
        cpp_schema_encoded = target_location_store->published_terminal == node_terminal;
    }
    source_location_store->terminal_override = node_terminal;
    const auto node_terminal_replay =
      actors.get_or_create (zlink::framework::actor_id_t ("actor-canonical"), "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    const auto *node_created =
      node_terminal_replay
        ? std::get_if<zlink::framework::actor_create_created_t> (&node_terminal_replay.value ())
        : nullptr;
    const bool node_schema_decoded =
      node_created && node_created->actor.actor_id ().value () == "actor-canonical"
      && node_created->actor.object_generation () == 1;

    source_location_store->terminal_override.reset ();
    target_location_store->hide_terminal_reads.store (true, std::memory_order_release);
    target_location_store->force_terminal_write_conflict.store (true, std::memory_order_release);
    const auto exceptional_terminal_reads_before =
      source_location_store->terminal_reads.load (std::memory_order_acquire);
    const auto exceptional_reply_replay =
      actors
        .get_or_create (zlink::framework::actor_id_t ("actor-exception-replay"),
                        "remote-create-actor")
        .timeout (std::chrono::seconds (5))
        .async ()
        .result ();
    const auto *exception_created =
      exceptional_reply_replay
        ? std::get_if<zlink::framework::actor_create_created_t> (&exceptional_reply_replay.value ())
        : nullptr;
    const bool exceptional_reply_replayed =
      exception_created && exception_created->actor.actor_id ().value () == "actor-exception-replay"
      && exception_created->actor.object_generation () == 2
      && source_location_store->terminal_reads.load (std::memory_order_acquire)
           == exceptional_terminal_reads_before + 2;

    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    source_thread.join ();
    target_thread.join ();

    const bool passed =
      route_ready && created && authority_active && cpp_schema_encoded && node_schema_decoded
      && exceptional_reply_replayed && source_terminal_reads == 1
      && std::holds_alternative<zlink::framework::actor_create_created_t> (created.value ())
      && remote_create_entry_spot_t::created_count.load (std::memory_order_acquire) == 2
      && remote_create_entry_spot_t::joined_count.load (std::memory_order_acquire) == 0
      && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
      && source_exit_code == 0 && target_exit_code == 0;
    if (!passed) {
        std::cerr << "remote Actor create target must publish the terminal before replying: "
                     "route-ready="
                  << route_ready
                  << " created-callback=" << remote_create_entry_spot_t::created_count.load ()
                  << " joined-callback=" << remote_create_entry_spot_t::joined_count.load ()
                  << " created=" << static_cast<bool> (created)
                  << " authority-active=" << authority_active
                  << " cpp-schema-encoded=" << cpp_schema_encoded
                  << " node-schema-decoded=" << node_schema_decoded
                  << " exceptional-reply-replayed=" << exceptional_reply_replayed
                  << " exceptional-result=" << static_cast<bool> (exceptional_reply_replay)
                  << " exceptional-actor="
                  << (exception_created ? exception_created->actor.actor_id ().value () : "-")
                  << " exceptional-generation="
                  << (exception_created ? exception_created->actor.object_generation () : 0)
                  << " exceptional-error="
                  << (exceptional_reply_replay.error () ? exceptional_reply_replay.error ()->what ()
                                                        : "-")
                  << " source-terminal-reads=" << source_terminal_reads
                  << " error=" << (created.error () ? created.error ()->what () : "-") << '\n';
    }
    return passed;
}

class mesh_started_probe_service_t final : public zlink::framework::hosted_service_t
{
  public:
    mesh_started_probe_service_t (zlink::framework::app_t &app, std::string mesh_name) :
        _app (app), _mesh_name (std::move (mesh_name))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        (void) services.get_required<zlink::framework::route_mesh_runtime_t> ().snapshot (
          _mesh_name);
        started = true;
        _app.stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool started = false;

  private:
    zlink::framework::app_t &_app;
    std::string _mesh_name;
};

class recovering_location_store_t final : public zlink::framework::location_store_t
{
  public:
    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        if (!_available.load (std::memory_order_acquire))
            return failed<zlink::framework::store_read_result_t> ();
        return _inner->read (std::move (key));
    }

    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        if (!_available.load (std::memory_order_acquire))
            return failed<zlink::framework::store_write_result_t> ();
        return _inner->write (std::move (request));
    }

    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

    void recover () noexcept { _available.store (true, std::memory_order_release); }

  private:
    template <typename T> static zlink::framework::task_t<T> failed ()
    {
        return zlink::framework::task_t<T> (zlink::framework::result_t<T>::failure (
          zlink::framework::framework_error_kind_t::unavailable, "injected Location Store outage"));
    }

    std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> _inner =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    std::atomic_bool _available{false};
};

class degraded_location_host_probe_t final : public zlink::framework::hosted_service_t
{
  public:
    degraded_location_host_probe_t (zlink::framework::app_t &app,
                                    std::shared_ptr<recovering_location_store_t> store,
                                    std::chrono::steady_clock::time_point started_at,
                                    std::chrono::milliseconds renew_timeout) :
        _app (app),
        _store (std::move (store)),
        _started_at (started_at),
        _renew_timeout (renew_timeout)
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto *location_repository =
          &services.get_required<zlink::framework::location_repository_t> ();
        auto manager = services.get_required<zlink::framework::spot_manager_t> ();
        auto *serializers = &services.get_required<zlink::framework::serializer_registry_t> ();
        _worker = std::thread ([this, location_repository, manager = std::move (manager),
                                serializers] () mutable {
            auto &locations = *location_repository;
            auto client = _app.advanced ().zlink ().route_client (*serializers);
            startup_completed =
              wait_until ([this] { return _app.is_ready (); }, std::chrono::seconds (1));
            started_within_timeout =
              std::chrono::steady_clock::now () - _started_at < _renew_timeout;
            descriptor_blocked =
              locations.list_mesh_nodes ("degraded-location-host").result ().value ().items.empty ()
              && locations.list_client_servers ("degraded-client-server")
                   .result ()
                   .value ()
                   .items.empty ()
              && locations.list_fanout_publishers ("degraded-fanout")
                   .result ()
                   .value ()
                   .items.empty ();

            const auto blocked_create =
              manager
                .get_or_create (zlink::framework::spot_id_t ("degraded-object"), "degraded-object")
                .in_mesh ("degraded-location-host")
                .timeout (std::chrono::milliseconds (50))
                .async ()
                .result ();
            factory_blocked =
              !blocked_create
              && degraded_object_spot_t::factory_count.load (std::memory_order_acquire) == 0;
            const auto blocked_message =
              client
                .send_to_spot (zlink::framework::spot_id_t ("degraded-object"),
                               degraded_object_message_t{17})
                .async ()
                .result ();
            message_and_timer_blocked =
              !blocked_message
              && degraded_object_spot_t::message_value.load (std::memory_order_acquire) == 0
              && degraded_object_timer_handler_t::timer_count.load (std::memory_order_acquire) == 0;
            _store->recover ();
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
            while (std::chrono::steady_clock::now () < deadline) {
                const auto mesh =
                  locations.list_mesh_nodes ("degraded-location-host").result ().value ();
                const auto client_server =
                  locations.list_client_servers ("degraded-client-server").result ().value ();
                const auto fanout =
                  locations.list_fanout_publishers ("degraded-fanout").result ().value ();
                mesh_republished = !mesh.items.empty () && !mesh.items.front ().owner_id.empty ()
                                   && mesh.items.front ().lease_generation > 0;
                client_server_republished = !client_server.items.empty ()
                                            && !client_server.items.front ().owner_id.empty ()
                                            && client_server.items.front ().lease_generation > 0;
                fanout_republished = !fanout.items.empty ()
                                     && !fanout.items.front ().owner_id.empty ()
                                     && fanout.items.front ().lease_generation > 0;
                if (mesh_republished && client_server_republished && fanout_republished) {
                    descriptor_republished = true;
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            const auto resumed_create =
              manager
                .get_or_create (zlink::framework::spot_id_t ("degraded-object"), "degraded-object")
                .in_mesh ("degraded-location-host")
                .timeout (std::chrono::seconds (1))
                .async ()
                .result ();
            factory_resumed =
              resumed_create
              && degraded_object_spot_t::factory_count.load (std::memory_order_acquire) == 1;
            const auto resumed_message =
              client
                .send_to_spot (zlink::framework::spot_id_t ("degraded-object"),
                               degraded_object_message_t{17})
                .async ()
                .result ();
            message_resumed = static_cast<bool> (resumed_message);
            if (!resumed_message)
                resumed_message_error = resumed_message.error () ? resumed_message.error ()->what ()
                                                                 : "message submission failed";
            message_and_timer_resumed =
              resumed_message
              && wait_until (
                [] {
                    return degraded_object_spot_t::message_value.load (std::memory_order_acquire)
                             == 17
                           && degraded_object_timer_handler_t::timer_count.load (
                                std::memory_order_acquire)
                                > 0;
                },
                std::chrono::seconds (1));
            _app.stop ();
        });
        co_return;
    }

    void stop () noexcept override
    {
        if (_worker.joinable ())
            _worker.join ();
    }

    bool startup_completed = false;
    bool started_within_timeout = false;
    bool descriptor_blocked = false;
    bool descriptor_republished = false;
    bool mesh_republished = false;
    bool client_server_republished = false;
    bool fanout_republished = false;
    bool factory_blocked = false;
    bool factory_resumed = false;
    bool message_and_timer_blocked = false;
    bool message_and_timer_resumed = false;
    bool message_resumed = false;
    std::string resumed_message_error;

  private:
    zlink::framework::app_t &_app;
    std::shared_ptr<recovering_location_store_t> _store;
    std::chrono::steady_clock::time_point _started_at;
    std::chrono::milliseconds _renew_timeout;
    std::thread _worker;
};

bool verify_degraded_host_republishes_descriptor_after_owner_claim ()
{
    degraded_object_spot_t::factory_count.store (0, std::memory_order_release);
    degraded_object_spot_t::message_value.store (0, std::memory_order_release);
    degraded_object_timer_handler_t::timer_count.store (0, std::memory_order_release);
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<recovering_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    constexpr auto renew_timeout = std::chrono::seconds (1);
    auto &options = app.add_zlink_framework ();
    options.add_location_store (store);
    options.add_relocation_store (relocation_store);
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval = std::chrono::milliseconds (10);
    locations.owner_lease_renew_timeout = renew_timeout;
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.owner_lease_fencing_margin = std::chrono::seconds (1);
    locations.polling_interval = std::chrono::milliseconds (5);
    auto degraded_mesh = options.add_route_mesh ("degraded-location-host");
    degraded_mesh.channel_name ("degraded-object-channel").server ();
    degraded_mesh.set_object_role (zlink::framework::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("degraded-location-host-node"))
      .listen ("inproc://degraded-location-host-node")
      .add_spot_factory<degraded_object_spot_t> (
        "degraded-object",
        [] (zlink::framework::spot_context_t context) {
            degraded_object_spot_t::factory_count.fetch_add (1, std::memory_order_acq_rel);
            return std::make_shared<degraded_object_spot_t> (std::move (context));
        },
        [] (auto &factory) { factory.recreate_on_relocation (); });
    options.add_client_server_channel ("degraded-client-server")
      .server ()
      .listen (0)
      .add_send_handler<degraded_channel_handler_t, degraded_channel_message_t> ();
    options.add_fanout_channel ("degraded-fanout")
      .enable_publisher (0)
      .set_routing_id (zlink::routing_id_t::from ("degraded-fanout-publisher"));

    const auto started_at = std::chrono::steady_clock::now ();
    auto probe =
      std::make_unique<degraded_location_host_probe_t> (app, store, started_at, renew_timeout);
    auto *probe_view = probe.get ();
    app.add_hosted_service (std::move (probe));
    char program[] = "degraded-location-host";
    char *arguments[] = {program, nullptr};
    const auto exit_code = app.run (1, arguments);
    if (exit_code == 0 && probe_view->startup_completed && probe_view->started_within_timeout
        && probe_view->descriptor_blocked && probe_view->descriptor_republished
        && probe_view->factory_blocked && probe_view->factory_resumed
        && probe_view->message_and_timer_blocked && probe_view->message_and_timer_resumed)
        return true;
    std::cerr << "degraded Location host did not gate and republish its descriptor"
              << " started-within-timeout=" << probe_view->started_within_timeout
              << " startup-completed=" << probe_view->startup_completed
              << " blocked=" << probe_view->descriptor_blocked
              << " republished=" << probe_view->descriptor_republished
              << " mesh=" << probe_view->mesh_republished
              << " client-server=" << probe_view->client_server_republished
              << " fanout=" << probe_view->fanout_republished
              << " factory-blocked=" << probe_view->factory_blocked
              << " factory-resumed=" << probe_view->factory_resumed
              << " work-blocked=" << probe_view->message_and_timer_blocked
              << " work-resumed=" << probe_view->message_and_timer_resumed
              << " send-resumed=" << probe_view->message_resumed
              << " message-value=" << degraded_object_spot_t::message_value.load ()
              << " timer-count=" << degraded_object_timer_handler_t::timer_count.load ()
              << " message-error=" << probe_view->resumed_message_error << '\n';
    return false;
}

bool verify_deferred_framework_apply_preserves_hosted_service_order ()
{
    constexpr std::string_view mesh_name = "deferred-framework-order-mesh";
    auto app = zlink::framework::app_t::create ();
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto &options = app.add_zlink_framework ();
    options.add_location_store (location_store);
    options.add_relocation_store (relocation_store);
    auto mesh = options.add_route_mesh (std::string (mesh_name));
    mesh.set_object_role (zlink::framework::object_role_t::none)
      .set_routing_id (zlink::routing_id_t::from ("deferred-framework-order-node"))
      .listen ("inproc://deferred-framework-order-node");

    auto probe = std::make_unique<mesh_started_probe_service_t> (app, std::string (mesh_name));
    auto *probe_view = probe.get ();
    app.add_hosted_service (std::move (probe));

    char program[] = "deferred-framework-order";
    char *arguments[] = {program, nullptr};
    try {
        const auto exit_code = app.run (1, arguments);
        if (exit_code == 0 && probe_view->started)
            return true;
    }
    catch (const std::exception &error) {
        std::cerr << "deferred Framework hosted-service order failed: " << error.what () << '\n';
        return false;
    }
    std::cerr << "deferred Framework hosted service did not start before the application service\n";
    return false;
}

bool expect_not_configured (
  std::string_view label,
  std::function<void (zlink::framework::zlink_framework_options_t &)> configure)
{
    auto app = zlink::framework::app_t::create ();
    try {
        app.add_zlink_framework (std::move (configure));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () == zlink::framework::framework_error_kind_t::not_configured)
            return true;
        std::cerr << label << " must fail with not_configured\n";
        return false;
    }
    std::cerr << label << " must fail before listener startup\n";
    return false;
}

bool verify_object_store_configuration_preflight ()
{
    auto unselected = zlink::framework::app_t::create ();
    try {
        unselected.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
            options.add_route_mesh ("unselected-object-role")
              .listen ("inproc://unselected-object-role");
        });
    }
    catch (const std::exception &error) {
        std::cerr << "unselected Object role must not require a Location Store: " << error.what ()
                  << '\n';
        return false;
    }

    if (!expect_not_configured ("Object Client without Location Store",
                                [] (zlink::framework::zlink_framework_options_t &options) {
                                    options.add_route_mesh ("missing-client-location")
                                      .set_object_role (zlink::framework::object_role_t::client)
                                      .listen ("inproc://missing-client-location");
                                }))
        return false;
    if (!expect_not_configured ("Object Server without Location Store",
                                [] (zlink::framework::zlink_framework_options_t &options) {
                                    options.add_route_mesh ("missing-server-location")
                                      .set_object_role (zlink::framework::object_role_t::server)
                                      .listen ("inproc://missing-server-location");
                                }))
        return false;

    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    if (!expect_not_configured (
          "relocatable Actor without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-actor-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-actor-relocation")
                .add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                  "configuration-actor", std::make_shared<configuration_actor_factory_t> (),
                  [] (auto &factory) { factory.recreate_on_relocation (); });
          }))
        return false;
    if (!expect_not_configured (
          "relocatable User Spot without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-spot-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-spot-relocation")
                .add_spot_factory<relocation_ready_spot_t> (
                  "configuration-spot",
                  [] (zlink::framework::spot_context_t context) {
                      return std::make_shared<relocation_ready_spot_t> (std::move (context));
                  },
                  [] (auto &factory) { factory.recreate_on_relocation (); });
          }))
        return false;
    if (!expect_not_configured (
          "Instance Spot without Relocation Store",
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              options.add_route_mesh ("missing-instance-relocation")
                .set_object_role (zlink::framework::object_role_t::server)
                .listen ("inproc://missing-instance-relocation")
                .add_instance_spot_factory<configuration_instance_spot_t> (
                  "configuration-instance",
                  [] (zlink::framework::instance_spot_context_t context) {
                      return std::make_shared<configuration_instance_spot_t> (std::move (context));
                  },
                  [] (auto &factory) { factory.disable_relocation (); });
          }))
        return false;

    auto disabled = zlink::framework::app_t::create ();
    try {
        disabled.add_zlink_framework (
          [location_store] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (location_store);
              auto mesh = options.add_route_mesh ("disabled-relocation");
              mesh.set_object_role (zlink::framework::object_role_t::server);
              mesh.listen ("inproc://disabled-relocation");
              mesh.add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
                "disabled-actor", std::make_shared<configuration_actor_factory_t> (),
                [] (auto &factory) { factory.disable_relocation (); });
              mesh.add_spot_factory<relocation_ready_spot_t> (
                "disabled-spot",
                [] (zlink::framework::spot_context_t context) {
                    return std::make_shared<relocation_ready_spot_t> (std::move (context));
                },
                [] (auto &factory) { factory.disable_relocation (); });
          });
    }
    catch (const std::exception &error) {
        std::cerr << "disabled factories must not require a Relocation Store: " << error.what ()
                  << '\n';
        return false;
    }
    return true;
}

void configure_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> &location_store,
  const std::shared_ptr<zlink::framework::runtime::in_memory_relocation_store_t> &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework (
      [location_store, relocation_store,
       routing_id = std::move (routing_id)] (zlink::framework::zlink_framework_options_t &options) {
          options.add_location_store (location_store);
          options.add_relocation_store (relocation_store);
          options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
          auto mesh = options.add_route_mesh ("host-relocation-mesh");
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

class blocking_stop_service_t final : public zlink::framework::hosted_service_t
{
  public:
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        {
            std::lock_guard lock (_mutex);
            _started = true;
        }
        _changed.notify_all ();
        co_return;
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
  zlink::framework::relocation_reason_t expected,
  std::chrono::milliseconds relocation_deadline = std::chrono::seconds (1),
  std::chrono::milliseconds minimum_wait = std::chrono::milliseconds::zero ())
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
    const auto serving_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!app.is_ready () && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto relocation_started_at = std::chrono::steady_clock::now ();
    const auto result =
      app
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = relocation_deadline})
        .result ()
        .value ();
    const auto relocation_elapsed = std::chrono::steady_clock::now () - relocation_started_at;
    const bool matched = result.outcome == zlink::framework::relocation_outcome_t::blocked
                         && result.reason == expected && app.is_ready ()
                         && relocation_elapsed >= minimum_wait;
    if (!matched)
        std::cerr << label << " must block Relocate without changing Serving\n";

    auto shutdown = app.shutdown (std::chrono::seconds (2));
    service_view->wait_stop_entered ();
    service_view->release ();
    const auto stopped = shutdown.result ().value ();
    run_thread.join ();
    return matched && stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && exit_code == 0;
}

bool verify_degraded_host_blocks_and_resumes_relocation ()
{
    auto app = zlink::framework::app_t::create ();
    auto location_store = std::make_shared<recovering_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto &options = app.add_zlink_framework ();
    options.add_location_store (location_store);
    options.add_relocation_store (relocation_store);
    auto &locations = options.configure_locations ();
    locations.owner_lease_renew_interval = std::chrono::milliseconds (10);
    locations.owner_lease_renew_timeout = std::chrono::milliseconds (50);
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.owner_lease_fencing_margin = std::chrono::seconds (1);
    locations.polling_interval = std::chrono::milliseconds (5);
    auto degraded_mesh = options.add_route_mesh ("degraded-relocation-host");
    degraded_mesh.channel_name ("degraded-relocation-channel").server ();
    degraded_mesh.set_object_role (zlink::framework::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("degraded-relocation-node"))
      .listen ("inproc://degraded-relocation-node")
      .add_spot_factory<degraded_object_spot_t> (
        "degraded-object",
        [] (zlink::framework::spot_context_t context) {
            return std::make_shared<degraded_object_spot_t> (std::move (context));
        },
        [] (auto &factory) { factory.recreate_on_relocation (); });
    auto service = std::make_unique<blocking_stop_service_t> ();
    auto *service_view = service.get ();
    app.add_hosted_service (std::move (service));

    char program[] = "degraded-relocation-host";
    char *arguments[] = {program, nullptr};
    int exit_code = -1;
    std::thread run_thread ([&] { exit_code = app.run (1, arguments); });
    service_view->wait_started ();
    const bool startup_completed =
      wait_until ([&] { return app.is_ready (); }, std::chrono::seconds (1));
    const auto blocked =
      app
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::milliseconds (50)})
        .result ()
        .value ();
    const bool relocation_blocked =
      blocked.outcome == zlink::framework::relocation_outcome_t::blocked
      && blocked.reason == zlink::framework::relocation_reason_t::store_unavailable;

    location_store->recover ();
    bool relocation_resumed = false;
    const auto recovery_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    do {
        const auto resumed =
          app
            .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = std::chrono::milliseconds (25)})
            .result ()
            .value ();
        relocation_resumed =
          resumed.outcome == zlink::framework::relocation_outcome_t::blocked
          && resumed.reason == zlink::framework::relocation_reason_t::target_unavailable;
        if (!relocation_resumed)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    } while (!relocation_resumed && std::chrono::steady_clock::now () < recovery_deadline);

    auto shutdown = app.shutdown (std::chrono::seconds (1));
    service_view->wait_stop_entered ();
    service_view->release ();
    const auto stopped = shutdown.result ().value ();
    run_thread.join ();
    const bool passed = startup_completed && relocation_blocked && relocation_resumed
                        && stopped.outcome == zlink::framework::termination_outcome_t::stopped
                        && exit_code == 0;
    if (!passed) {
        std::cerr << "degraded host relocation admission did not close and resume"
                  << " startup=" << startup_completed
                  << " blocked-reason=" << static_cast<int> (blocked.reason)
                  << " blocked=" << relocation_blocked << " resumed=" << relocation_resumed << '\n';
    }
    return passed;
}

void configure_empty_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> &location_store,
  const std::shared_ptr<zlink::framework::runtime::in_memory_relocation_store_t> &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework (
      [location_store, relocation_store,
       routing_id = std::move (routing_id)] (zlink::framework::zlink_framework_options_t &options) {
          options.add_location_store (location_store);
          options.add_relocation_store (relocation_store);
          options.configure_locations ().polling_interval = std::chrono::milliseconds (10);
          auto mesh = options.add_route_mesh ("host-relocation-retry-mesh");
          mesh.channel_name ("host-relocation-retry-channel").server ();
          mesh.set_object_role (zlink::framework::object_role_t::server)
            .listen ("tcp://127.0.0.1:0")
            .set_routing_id (zlink::routing_id_t::from (routing_id));
      });
}

bool verify_relocation_retry_after_target_unavailable ()
{
    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();

    auto source = zlink::framework::app_t::create ();
    configure_empty_relocation_app (source, location_store, relocation_store,
                                    "host-relocation-retry-source");
    char source_program[] = "host-relocation-retry-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] { source_exit_code = source.run (1, source_arguments); });
    if (!wait_until ([&] { return source.is_ready (); }, std::chrono::seconds (2))) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "relocation retry source must reach Serving\n";
        return false;
    }

    const auto first =
      source
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::milliseconds (100)})
        .result ()
        .value ();
    if (first.outcome != zlink::framework::relocation_outcome_t::blocked
        || first.reason != zlink::framework::relocation_reason_t::target_unavailable
        || !source.is_ready ()) {
        source.request_stop ();
        source_thread.join ();
        std::cerr << "first relocation must return Blocked/TargetUnavailable "
                     "and preserve Serving\n";
        return false;
    }

    auto target = zlink::framework::app_t::create ();
    configure_empty_relocation_app (target, location_store, relocation_store,
                                    "host-relocation-retry-target");
    char target_program[] = "host-relocation-retry-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] { target_exit_code = target.run (1, target_arguments); });
    if (!wait_until ([&] { return target.is_ready (); }, std::chrono::seconds (2))) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        std::cerr << "relocation retry target must reach Serving\n";
        return false;
    }

    const auto second =
      source
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::seconds (3)})
        .result ()
        .value ();
    const auto target_stopped = target.shutdown (std::chrono::seconds (2)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (2)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    if (second.outcome != zlink::framework::relocation_outcome_t::relocated
        || second.reason != zlink::framework::relocation_reason_t::none) {
        std::cerr << "second relocation must restart preflight and relocate: outcome="
                  << static_cast<int> (second.outcome)
                  << " reason=" << static_cast<int> (second.reason) << '\n';
        return false;
    }
    return target_stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && source_stopped.outcome == zlink::framework::termination_outcome_t::stopped
           && target_exit_code == 0 && source_exit_code == 0;
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
          return source_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0
                 && target_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0;
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
          return source_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0
                 && target_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0;
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
          return source_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0
                 && target_mesh.snapshot ("host-relocation-mesh").ready_peer_count > 0;
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

bool verify_application_signaled_relocation ()
{
    relocation_ready_spot_t::completion_count.store (0, std::memory_order_release);
    relocation_ready_spot_t::last_outcome.store (-1, std::memory_order_release);
    relocation_ready_adapter_t::restored.store (false, std::memory_order_release);

    auto location_store =
      std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store =
      std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto submit_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_relocation_app (source, location_store, relocation_store,
                              "zz-host-relocation-source");
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
          std::chrono::seconds (7))) {
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
    char target_program[] = "host-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] {
        /* Relocate starts before this replacement publishes its descriptor.
         * The source must keep checking Store and Core peer readiness until
         * the shared deadline instead of rejecting the first empty snapshot. */
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        target_exit_code = target.run (1, target_arguments);
    });

    auto relocation =
      source.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                        .deadline = std::chrono::seconds (10)});
    submit_ready->store (true, std::memory_order_release);
    const auto result = relocation.result ().value ();

    const auto target_stopped = target.shutdown (std::chrono::seconds (5)).result ().value ();
    const auto source_stopped = source.shutdown (std::chrono::seconds (5)).result ().value ();
    target_thread.join ();
    source_thread.join ();

    const bool passed =
      source_service_view->ready_sent.load (std::memory_order_acquire)
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

bool verify_relocation_target_eligibility_applies_full_narrowing ()
{
    using zlink::framework::framework_runtime_state_t;
    using zlink::framework::maintenance_policy_kind_t;
    using zlink::framework::mesh_node_descriptor_t;
    using zlink::framework::object_capability_t;
    using zlink::framework::object_role_t;
    using zlink::framework::placement_object_kind_t;
    using zlink::framework::relocation_unit_target_eligible;

    mesh_node_descriptor_t source;
    source.application_version = 7;
    source.maintenance_wave = "wave-a";
    source.object_capabilities.push_back (object_capability_t{
      placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::disabled, false, 0});
    source.object_capabilities.push_back (object_capability_t{
      placement_object_kind_t::actor, "player", maintenance_policy_kind_t::disabled, false, 0});

    const auto make_eligible = [] () {
        mesh_node_descriptor_t candidate;
        candidate.state = framework_runtime_state_t::serving;
        candidate.object_role = object_role_t::server;
        candidate.placement_weight = 100;
        candidate.application_version = 7;
        candidate.object_capabilities.push_back (
          object_capability_t{placement_object_kind_t::user_spot, "room",
                              maintenance_policy_kind_t::disabled, false, 0});
        candidate.object_capabilities.push_back (object_capability_t{
          placement_object_kind_t::actor, "player", maintenance_policy_kind_t::disabled, false, 0});
        return candidate;
    };

    if (!relocation_unit_target_eligible (source, make_eligible (), 7, "room", {"player"})) {
        std::cerr << "eligible relocation target was wrongly rejected\n";
        return false;
    }

    // The pre-fix selector predicate checked only serving state, version, and
    // object-type support, so it accepted every candidate below. The frozen
    // 6-step narrowing (cpp interface 02-configuration-host) must reject them.
    struct rejection_case_t
    {
        const char *name;
        std::function<void (mesh_node_descriptor_t &)> mutate;
    };
    const rejection_case_t rejection_cases[] = {
      {"zero placement weight", [] (mesh_node_descriptor_t &c) { c.placement_weight = 0; }},
      {"not an Object Server",
       [] (mesh_node_descriptor_t &c) { c.object_role = object_role_t::none; }},
      {"same maintenance wave as source",
       [] (mesh_node_descriptor_t &c) { c.maintenance_wave = "wave-a"; }},
      {"exhausted actor capacity",
       [] (mesh_node_descriptor_t &c) {
           c.capacity.actors.limit = 1;
           c.capacity.actors.active = 1;
       }},
      {"wrong effective version", [] (mesh_node_descriptor_t &c) { c.application_version = 8; }},
    };
    for (const auto &item : rejection_cases) {
        auto candidate = make_eligible ();
        item.mutate (candidate);
        if (relocation_unit_target_eligible (source, candidate, 7, "room", {"player"})) {
            std::cerr << "ineligible relocation target was accepted: " << item.name << "\n";
            return false;
        }
    }

    // Snapshot relocation policy requires a snapshot adapter on the candidate.
    {
        mesh_node_descriptor_t snapshot_source = source;
        snapshot_source.object_capabilities[0].policy = maintenance_policy_kind_t::snapshot;
        auto candidate = make_eligible ();
        candidate.object_capabilities[0].policy = maintenance_policy_kind_t::snapshot;
        candidate.object_capabilities[0].has_snapshot_adapter = false;
        if (relocation_unit_target_eligible (snapshot_source, candidate, 7, "room", {"player"})) {
            std::cerr << "snapshot candidate without adapter was accepted\n";
            return false;
        }
        candidate.object_capabilities[0].has_snapshot_adapter = true;
        if (!relocation_unit_target_eligible (snapshot_source, candidate, 7, "room", {"player"})) {
            std::cerr << "snapshot candidate with adapter was rejected\n";
            return false;
        }
    }

    // An actor-only unit (empty Spot type) imposes no user Spot requirement but
    // still applies the per-candidate gates.
    {
        auto candidate = make_eligible ();
        if (!relocation_unit_target_eligible (source, candidate, 7, "", {"player"})) {
            std::cerr << "eligible actor-only target was rejected\n";
            return false;
        }
        candidate.placement_weight = 0;
        if (relocation_unit_target_eligible (source, candidate, 7, "", {"player"})) {
            std::cerr << "ineligible actor-only target (zero weight) was accepted\n";
            return false;
        }
    }

    // Spec step 6: deterministic weighted draw over the eligible set. A higher
    // weight is preferred, the result is order-independent, and a zero weight is
    // excluded.
    {
        using zlink::framework::select_weighted_relocation_target;
        using weighted_set_t = std::vector<std::pair<std::string, std::uint32_t>>;

        const weighted_set_t high_low = {{"high", 3}, {"low", 1}};
        if (select_weighted_relocation_target (high_low) != std::optional<std::string> ("high")) {
            std::cerr << "weighted draw did not prefer the higher weight\n";
            return false;
        }
        const weighted_set_t low_high = {{"low", 1}, {"high", 3}};
        if (select_weighted_relocation_target (low_high) != std::optional<std::string> ("high")) {
            std::cerr << "weighted draw was not order-independent\n";
            return false;
        }
        const weighted_set_t single = {{"only", 1}};
        if (select_weighted_relocation_target (single) != std::optional<std::string> ("only")) {
            std::cerr << "weighted draw did not return the sole candidate\n";
            return false;
        }
        const weighted_set_t empty_set = {};
        if (select_weighted_relocation_target (empty_set).has_value ()) {
            std::cerr << "weighted draw returned a target for an empty set\n";
            return false;
        }
        const weighted_set_t zero_and_positive = {{"zero", 0}, {"positive", 2}};
        if (select_weighted_relocation_target (zero_and_positive)
            != std::optional<std::string> ("positive")) {
            std::cerr << "weighted draw did not exclude the zero-weight candidate\n";
            return false;
        }
    }

    return true;
}

} // namespace

int main ()
{
    if (!verify_degraded_host_republishes_descriptor_after_owner_claim ())
        return EXIT_FAILURE;

    if (!verify_degraded_host_blocks_and_resumes_relocation ())
        return EXIT_FAILURE;

    if (!verify_deferred_framework_apply_preserves_hosted_service_order ())
        return EXIT_FAILURE;

    if (!verify_object_store_configuration_preflight ())
        return EXIT_FAILURE;

    if (!verify_relocation_target_eligibility_applies_full_narrowing ())
        return EXIT_FAILURE;

    if (!verify_remote_actor_create_target_owns_completion ())
        return EXIT_FAILURE;

    if (!verify_relocation_retry_after_target_unavailable ())
        return EXIT_FAILURE;

    if (!verify_relocating_status_closes_admission ())
        return EXIT_FAILURE;

    if (!verify_relocation_join_ignores_deadline ())
        return EXIT_FAILURE;

    if (!verify_relocation_reject_reports_requested_target ())
        return EXIT_FAILURE;

    if (std::getenv ("ZLINK_CPP_RUN_HOST_RELOCATION_INTEGRATION")
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
              node.set_object_role (zlink::framework::object_role_t::none);
              node.channel_name ("retire-manual-channel").client ();
              node.set_routing_id (zlink::routing_id_t::from ("retire-manual-node"))
                .listen ("inproc://cpp-retire-manual-node");
              node.peer_connections ().connect ("tcp://127.0.0.1:29998");
          },
          zlink::framework::relocation_reason_t::manual_topology_unsupported)) {
        return EXIT_FAILURE;
    }
    if (!verify_relocation_blocker (
          "automatic RouteMesh without a replacement",
          [] (zlink::framework::zlink_framework_options_t &options) {
              options.add_location_store (
                std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ());
              options.add_relocation_store (
                std::make_shared<zlink::framework::runtime::in_memory_relocation_store_t> ());
              auto node = options.add_route_mesh ("retire-single-mesh");
              node.set_object_role (zlink::framework::object_role_t::server);
              node.channel_name ("retire-single-channel").client ();
              node.set_routing_id (zlink::routing_id_t::from ("retire-single-node"))
                .listen ("inproc://cpp-retire-single-node");
          },
          zlink::framework::relocation_reason_t::target_unavailable, std::chrono::milliseconds (75),
          std::chrono::milliseconds (50))) {
        return EXIT_FAILURE;
    }

    auto app = zlink::framework::app_t::create ();
    if (app.runtime_state () != zlink::framework::framework_runtime_state_t::preparing) {
        std::cerr << "new app must begin in Preparing\n";
        return EXIT_FAILURE;
    }

    bool planned_target_rejected = false;
    try {
        (void) app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                              .target_application_version = 2});
    }
    catch (const std::invalid_argument &) {
        planned_target_rejected = true;
    }
    bool rolling_target_required = false;
    try {
        (void) app.relocate ({.mode = zlink::framework::relocation_mode_t::rolling_update});
    }
    catch (const std::invalid_argument &) {
        rolling_target_required = true;
    }
    if (!planned_target_rejected || !rolling_target_required) {
        std::cerr << "Relocate mode must validate its target version option\n";
        return EXIT_FAILURE;
    }

    const auto relocation =
      app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (relocation.outcome != zlink::framework::relocation_outcome_t::blocked
        || relocation.reason != zlink::framework::relocation_reason_t::runtime_not_ready) {
        std::cerr << "Relocate before Serving must return Blocked/RuntimeNotReady\n";
        return EXIT_FAILURE;
    }

    const auto shutdown = app.shutdown (std::chrono::seconds (1)).result ().value ();
    if (shutdown.outcome != zlink::framework::termination_outcome_t::stopped
        || shutdown.reason != zlink::framework::termination_reason_t::none
        || app.runtime_state () != zlink::framework::framework_runtime_state_t::stopped) {
        std::cerr << "Shutdown must complete the shared termination operation\n";
        return EXIT_FAILURE;
    }
    const auto after_shutdown =
      app.relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance})
        .result ()
        .value ();
    if (after_shutdown.outcome != zlink::framework::relocation_outcome_t::blocked
        || after_shutdown.reason != zlink::framework::relocation_reason_t::runtime_not_ready) {
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
    std::thread run_thread ([&] { exit_code = running.run (1, arguments); });
    service_view->wait_started ();
    const auto serving_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!running.is_ready () && std::chrono::steady_clock::now () < serving_deadline)
        std::this_thread::yield ();

    const auto unavailable_relocation =
      running
        .relocate ({.mode = zlink::framework::relocation_mode_t::planned_maintenance,
                    .deadline = std::chrono::seconds (2)})
        .result ()
        .value ();
    if (unavailable_relocation.outcome != zlink::framework::relocation_outcome_t::blocked
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
      running.shutdown (std::chrono::seconds (2), cancelled_source.get_token ());
    const auto &cancelled = cancelled_waiter.result ();
    if (cancelled || !cancelled.error ()
        || cancelled.error ()->code () != std::make_error_code (std::errc::operation_canceled)) {
        std::cerr << "wait cancellation must cancel only the joining waiter\n";
        service_view->release ();
        run_thread.join ();
        return EXIT_FAILURE;
    }

    service_view->release ();
    const auto shared_result = shared_shutdown.result ().value ();
    run_thread.join ();
    if (shared_result.outcome != zlink::framework::termination_outcome_t::stopped
        || exit_code != 0) {
        std::cerr << "shared Shutdown must survive waiter cancellation\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
