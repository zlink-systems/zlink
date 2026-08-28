/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Common/workflow_logic.hpp"
#include "../Configuration/sample_configuration.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace zlink::samples::shoppingmall
{
using namespace zlink::framework;

class order_workflow_spot_t;
class planned_relocation_workflow_spot_t;

struct order_workflow_continue_timer_handler_t
{
    void handle (order_workflow_spot_t &spot, const timer_tick_t &) const;
};

struct planned_relocation_readiness_timer_handler_t
{
    void handle (planned_relocation_workflow_spot_t &spot, const timer_tick_t &) const;
};

class order_workflow_spot_t : public instance_spot_t
{
  public:
    order_workflow_spot_t (instance_spot_context_t context,
                           sample_topology_t topology,
                           workflow_instance_topology_t instance) :
        _store (std::move (topology)), _context (std::move (context)), _instance (std::move (instance))
    {
    }

    instance_spot_context_t &context () noexcept override { return _context; }
    const instance_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_handler<&order_workflow_spot_t::start> (start_order_workflow_req_t::packet_name)
          .add_handler<&order_workflow_spot_t::continue_> (
            continue_order_workflow_req_t::packet_name)
          .add_handler<&order_workflow_spot_t::continue_scheduled> (
            continue_order_workflow_msg_t::packet_name)
          .add_handler<&order_workflow_spot_t::rebuild> (
            rebuild_order_projection_req_t::packet_name);
    }

    task_t<void> on_initialize () override
    {
        _order_id = _context.spot_id ();
        _store.update ([&] (nlohmann::json &state) {
            state["testHooks"]["activeWorkflowSpots"][_order_id] =
              nlohmann::json{{"node", _instance.instance_id},
                             {"generation", _context.object_generation ()}};
        });
        co_return;
    }

    task_t<void> on_closing (const spot_closing_context_t &, std::stop_token) override
    {
        _continue_timer.cancel ();
        co_return;
    }

    /* 공통 sample spec §9.3: 시작은 루프를 Created까지만 돌리고 즉시 응답하며, 나머지 단계를
     * 진행할 재개 호출을 기다리지 않고 예약한다. 결제 지연을 HTTP 응답에 묶지 않기 위해서다. */
    start_order_workflow_res_t start (const start_order_workflow_req_t &request)
    {
        auto state = _store.update ([&] (nlohmann::json &json) {
            return run_workflow (json, request.order_id, source_command_id (request), &request,
                                 /*max_steps=*/1);
        });
        std::cerr << "shoppingmall-order started order=" << state.order_id
                  << " spot=" << _order_id << "\n";
        if (state.status != order_status_t::confirmed && state.status != order_status_t::failed) {
            schedule_continue (state.order_id);
        }
        return {state};
    }

    /* 재개는 다음 단계가 없을 때까지 같은 루프를 돌린다. 시작이 예약한 호출이든, 복구용 외부
     * 호출이든 코드는 같다. */
    continue_order_workflow_res_t continue_ (const continue_order_workflow_req_t &request)
    {
        return {run_to_completion (request.order_id, request.source_command_id)};
    }

    void continue_scheduled (const continue_order_workflow_msg_t &message)
    {
        (void) run_to_completion (message.order_id);
    }

    rebuild_order_projection_res_t rebuild (const rebuild_order_projection_req_t &request)
    {
        auto state = _store.update (
          [&] (nlohmann::json &json) { return rebuild_projection (json, request.order_id); });
        std::cerr << "shoppingmall order: projection rebuilt order=" << state.order_id
                  << " status=" << state.status << "\n";
        return {state};
    }

    void run_scheduled_continue ()
    {
        _continue_timer.cancel ();
        auto order_id = std::move (_scheduled_order_id);
        _scheduled_order_id.clear ();
        if (!order_id.empty ()) {
            continue_scheduled (continue_order_workflow_msg_t{std::move (order_id)});
        }
    }

  private:
    static std::string source_command_id (const start_order_workflow_req_t &request)
    {
        /* 같은 IdempotencyKey의 시작 요청은 같은 SourceCommandId를 사용한다(§9.4). */
        return request.source_command_id.empty () ? "start:" + request.idempotency_key
                                                  : request.source_command_id;
    }

    order_state_t run_to_completion (const std::string &order_id,
                                     const std::string &source_command_id = {})
    {
        return _store.update ([&] (nlohmann::json &json) {
            return run_workflow (json, order_id, source_command_id, nullptr,
                                 /*max_steps=*/16);
        });
    }

    void schedule_continue (const std::string &order_id)
    {
        _continue_timer.cancel ();
        _scheduled_order_id = order_id;
        _continue_timer = _context.add_timer<order_workflow_continue_timer_handler_t> (
          "order-workflow-continue", std::chrono::milliseconds (1));
    }

    redis_state_store_t _store;
    instance_spot_context_t _context;
    workflow_instance_topology_t _instance;
    zlink::framework::timer_t _continue_timer;
    std::string _scheduled_order_id;
    std::string _order_id;
};

void order_workflow_continue_timer_handler_t::handle (order_workflow_spot_t &spot,
                                                      const timer_tick_t &) const
{
    spot.run_scheduled_continue ();
}

/* Runner-only operation request.  A dedicated workflow User Spot is the
 * relocation unit because host-level planned maintenance relocates User Spots;
 * the order Instance Spot remains the ordinary object-routing endpoint. */
struct planned_relocation_req_t
{
    static constexpr const char *packet_name = "PlannedRelocationReq";
    std::string order_id;
};

struct planned_relocation_res_t
{
    static constexpr const char *packet_name = "PlannedRelocationRes";
    bool accepted{};
    std::uint64_t generation{};
};

inline void to_json (nlohmann::json &json, const planned_relocation_req_t &value)
{
    json = {{"orderId", value.order_id}};
}

inline void from_json (const nlohmann::json &json, planned_relocation_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
}

inline void to_json (nlohmann::json &json, const planned_relocation_res_t &value)
{
    json = {{"accepted", value.accepted}, {"generation", value.generation}};
}

inline void from_json (const nlohmann::json &json, planned_relocation_res_t &value)
{
    value.accepted = json.value ("accepted", false);
    value.generation = json.value ("generation", std::uint64_t{});
}

class planned_relocation_workflow_spot_t final : public spot_t<actor_t>
{
  public:
    planned_relocation_workflow_spot_t (spot_context_t context,
                                        sample_topology_t topology,
                                        workflow_instance_topology_t instance) :
        _context (std::move (context)), _store (std::move (topology)), _instance (std::move (instance))
    {
    }

    spot_context_t &context () noexcept override { return _context; }
    const spot_context_t &context () const noexcept override { return _context; }

    void configure () override {}

    task_t<void> on_initialize () override
    {
        const auto fixture_id = std::string (_context.spot_id ());
        constexpr std::string_view prefix{"shoppingmall.planned-relocation:"};
        if (!fixture_id.starts_with (prefix)) co_return;
        const auto order_id = fixture_id.substr (prefix.size ());
        enum class initialization_role_t { ignored, source, target };
        const auto role = _store.update ([&] (nlohmann::json &state) {
            auto &planned = state["testHooks"]["plannedRelocation"];
            const auto found = planned.find (order_id);
            if (found == planned.end ()) return initialization_role_t::ignored;
            if (found->value ("fixtureGeneration", std::uint64_t{}) == 0) {
                (*found)["sourceNode"] = _instance.instance_id;
                (*found)["fixtureGeneration"] = _context.object_generation ();
                return initialization_role_t::source;
            }
            if (found->value ("sourceNode", std::string{}) == _instance.instance_id) {
                return initialization_role_t::ignored;
            }
            if (found->value ("fixtureGeneration", std::uint64_t{})
                  != _context.object_generation ()
                || found->value ("replayed", false)) {
                return initialization_role_t::ignored;
            }
            (*found)["replayed"] = true;
            return initialization_role_t::target;
        });
        if (role == initialization_role_t::source) {
            schedule_readiness_check ();
        } else if (role == initialization_role_t::target) {
            /* The target workflow fixture has been recreated by the planned
             * relocation with the same ObjectGeneration.  Replay decides the
             * next action, so InventoryReserved is not attempted again. */
            std::cerr << "shoppingmall-order replayed order=" << order_id
                      << " generation=" << _context.object_generation () << "\n";
            _store.update ([&] (nlohmann::json &state) {
                return run_workflow (state, order_id, "continue:planned-relocation", nullptr,
                                     /*max_steps=*/16);
            });
        }
        co_return;
    }

  public:
    void check_relocation_readiness ()
    {
        const auto fixture_id = std::string (_context.spot_id ());
        constexpr std::string_view prefix{"shoppingmall.planned-relocation:"};
        const auto order_id = fixture_id.starts_with (prefix) ? fixture_id.substr (prefix.size ())
                                                              : std::string{};
        const auto ready = !order_id.empty () && _store.read ([&] (const nlohmann::json &state) {
            const auto planned = state["testHooks"]["plannedRelocation"].find (order_id);
            return planned != state["testHooks"]["plannedRelocation"].end ()
                   && planned->value ("sourceNode", std::string{}) == _instance.instance_id
                   && planned->value ("operation", std::string{}) == "ready-for-defer";
        });
        if (!ready) {
            schedule_readiness_check ();
            return;
        }
        _context.relocation_ready ().defer ();
    }

    task_t<spot_create_response_t> on_create (const zlink::framework::message_t &) override
    {
        co_return spot_create_response_t::accept ();
    }

    task_t<spot_actor_join_result_t> on_actor_join (std::string_view,
                                                    const zlink::framework::message_t &) override
    {
        co_return spot_actor_join_result_t::reject ();
    }

    task_t<void> on_actor_joined (actor_t &) override { co_return; }
    task_t<void> on_leave_actor (actor_t &) override { co_return; }

  private:
    void schedule_readiness_check ()
    {
        _readiness_timer.cancel ();
        _readiness_timer = _context.add_timer<planned_relocation_readiness_timer_handler_t> (
          "planned-relocation-readiness", std::chrono::milliseconds (1));
    }

    spot_context_t _context;
    redis_state_store_t _store;
    workflow_instance_topology_t _instance;
    zlink::framework::timer_t _readiness_timer;
};

void planned_relocation_readiness_timer_handler_t::handle (
  planned_relocation_workflow_spot_t &spot, const timer_tick_t &) const
{
    spot.check_relocation_readiness ();
}

class planned_relocation_handler_t
{
  public:
    using request_type = planned_relocation_req_t;
    using reply_type = planned_relocation_res_t;
    using dependency_types =
      dependency_list_t<redis_state_store_t, workflow_instance_topology_t, spot_manager_t>;
    static constexpr const char *topic_name = request_type::packet_name;

    planned_relocation_handler_t (redis_state_store_t &store,
                                  workflow_instance_topology_t &instance,
                                  spot_manager_t &spots) :
        _store (store), _instance (instance), _spots (spots)
    {
    }

    task_t<planned_relocation_res_t> handle (const planned_relocation_req_t &request)
    {
        const auto fixture_id = _store.update ([&] (nlohmann::json &state) {
            const auto active = state["testHooks"]["activeWorkflowSpots"].find (request.order_id);
            if (active == state["testHooks"]["activeWorkflowSpots"].end ()) {
                return std::string{};
            }
            const auto generation = active->value ("generation", std::uint64_t{});
            if (generation == 0) {
                return std::string{};
            }
            const auto fixture = "shoppingmall.planned-relocation:" + request.order_id;
            state["testHooks"]["plannedRelocation"][request.order_id] =
              nlohmann::json{{"orderGeneration", generation},
                             {"fixtureSpotId", fixture},
                             {"operation", "requested"},
                             {"replayed", false}};
            return fixture;
        });
        if (fixture_id.empty ()) co_return planned_relocation_res_t{};
        try {
            const auto created =
              co_await _spots.get_or_create (spot_id_t (fixture_id),
                                             "shoppingmall.planned.relocation.workflow")
                .submit ();
            co_return planned_relocation_res_t{
              true, created.spot.object_generation ()};
        }
        catch (const framework_exception_t &error) {
            throw framework_exception_t (
              error.kind (), "planned relocation workflow fixture was not created");
        }
    }

  private:
    redis_state_store_t &_store;
    workflow_instance_topology_t &_instance;
    spot_manager_t &_spots;
};

class planned_relocation_service_t final : public hosted_service_t
{
  public:
    planned_relocation_service_t (app_t &app, sample_topology_t topology, std::string instance_id) :
        _app (app), _store (std::move (topology)), _instance_id (std::move (instance_id))
    {
    }

    task_t<void> start (service_provider_t &services) override
    {
        _runtime = &services.get_required<framework_runtime_t> ();
        _mesh = &services.get_required<route_mesh_runtime_t> ();
        _worker = std::thread ([this] { run (); });
        co_return;
    }

    void request_stop () noexcept override { _stopping.store (true, std::memory_order_release); }

    void stop () noexcept override
    {
        request_stop ();
        if (_worker.joinable ()) {
            _worker.join ();
        }
    }

  private:
    void run () noexcept
    {
        while (!_stopping.load (std::memory_order_acquire)) {
            std::string fixture_id;
            try {
                fixture_id = _store.update ([&] (nlohmann::json &state) {
                    auto &planned = state["testHooks"]["plannedRelocation"];
                    for (auto found = planned.begin (); found != planned.end (); ++found) {
                        if (found->value ("sourceNode", std::string{}) == _instance_id
                            && found->value ("operation", std::string{}) == "requested") {
                            (*found)["operation"] = "starting";
                            return found->value ("fixtureSpotId", std::string{});
                        }
                    }
                    return std::string{};
                });
                if (!fixture_id.empty ()) {
                    const auto peer_deadline = std::chrono::steady_clock::now ()
                                               + std::chrono::seconds (5);
                    while (!_stopping.load (std::memory_order_acquire)
                           && _mesh->snapshot (sample_names_t::order_workflow_channel)
                                  .ready_peer_count == 0
                           && std::chrono::steady_clock::now () < peer_deadline) {
                        std::this_thread::sleep_for (std::chrono::milliseconds (1));
                    }
                    if (_mesh->snapshot (sample_names_t::order_workflow_channel).ready_peer_count
                        == 0) {
                        throw framework_exception_t (
                          framework_error_kind_t::invalid_operation,
                          "planned relocation has no admitted workflow peer");
                    }
                    auto operation = _app.relocate ({.mode = relocation_mode_t::planned_maintenance,
                                                      .deadline = std::chrono::seconds (15)});
                    const auto readiness_deadline = std::chrono::steady_clock::now ()
                                                    + std::chrono::seconds (5);
                    while (!_stopping.load (std::memory_order_acquire)
                           && _runtime->status ().state
                                != framework_runtime_state_t::relocating
                           && std::chrono::steady_clock::now () < readiness_deadline) {
                        std::this_thread::sleep_for (std::chrono::milliseconds (1));
                    }
                    if (_runtime->status ().state != framework_runtime_state_t::relocating) {
                        throw framework_exception_t (
                          framework_error_kind_t::invalid_operation,
                          "planned relocation did not enter its readiness boundary");
                    }
                    _store.update ([&] (nlohmann::json &state) {
                        auto &planned = state["testHooks"]["plannedRelocation"];
                        for (auto found = planned.begin (); found != planned.end (); ++found) {
                            if (found->value ("fixtureSpotId", std::string{}) == fixture_id) {
                                (*found)["operation"] = "ready-for-defer";
                                break;
                            }
                        }
                    });
                    struct completion_t
                    {
                        std::condition_variable ready;
                        std::mutex mutex;
                        std::exception_ptr error;
                        bool completed = false;
                    };
                    auto completion = std::make_shared<completion_t> ();
                    observe_task_completion (
                      operation,
                      [completion] (const result_t<relocation_result_t> &result) {
                          std::exception_ptr error;
                          try {
                              (void) result.value ();
                          }
                          catch (...) {
                              error = std::current_exception ();
                          }
                          {
                              std::lock_guard lock (completion->mutex);
                              completion->error = std::move (error);
                              completion->completed = true;
                          }
                          completion->ready.notify_one ();
                      });
                    std::unique_lock lock (completion->mutex);
                    completion->ready.wait (
                      lock, [&completion] { return completion->completed; });
                    if (completion->error) {
                        std::rethrow_exception (completion->error);
                    }
                    return;
                }
            }
            catch (const std::exception &error) {
                std::cerr << "shoppingmall planned relocation failed: " << error.what () << "\n";
                return;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
    }

    app_t &_app;
    framework_runtime_t *_runtime = nullptr;
    route_mesh_runtime_t *_mesh = nullptr;
    redis_state_store_t _store;
    std::string _instance_id;
    std::atomic_bool _stopping{false};
    std::thread _worker;
};

} // namespace zlink::samples::shoppingmall

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::shoppingmall;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    auto instance = topology.for_workflow_instance (configuration.role.name);
    redis_state_store_t store{topology};
    store.seed_defaults ();
    app.logging ().use_file (configuration.flow_log_path ());
    auto &options = app.add_zlink_framework ();
    options.services ()
      .add_singleton<sample_topology_t> (std::make_unique<sample_topology_t> (topology))
      .add_singleton<workflow_instance_topology_t> (
        std::make_unique<workflow_instance_topology_t> (instance))
      .add_singleton<redis_state_store_t, sample_topology_t> ();
    options.add_location_store<redis::redis_location_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "location:");
    options.add_relocation_store<redis::redis_relocation_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "relocation:");
    options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
    const auto workflow_channel = sample_names_t::order_workflow_channel;
    auto workflow_route = options.add_route_mesh (workflow_channel);
    workflow_route
      .set_routing_id (
        zlink::routing_id_t::from ("shoppingmall-" + instance.instance_id + "-workflow"))
      .listen (instance.route_endpoint);
    workflow_route.objects ()
      .server ()
      .add_instance_spot_factory<order_workflow_spot_t, sample_topology_t,
                                 workflow_instance_topology_t> (
        sample_names_t::order_workflow_spot)
      .disable_relocation ()
      .add_spot_factory<planned_relocation_workflow_spot_t, sample_topology_t,
                        workflow_instance_topology_t> ("shoppingmall.planned.relocation.workflow")
      .set_execution_mode (user_spot_execution_mode_t::spot_wide)
      .set_relocation_coordination_mode (
        spot_relocation_coordination_mode_t::application_signaled)
      .recreate_on_relocation ();
    options.http ()
      .listen (instance.http_url)
      .map_health ("/health")
      .map_post<planned_relocation_handler_t> ("/self-check/relocation");
    app.add_hosted_service (std::make_unique<planned_relocation_service_t> (
      app, topology, instance.instance_id));
    return app.run (argc, argv);
}
