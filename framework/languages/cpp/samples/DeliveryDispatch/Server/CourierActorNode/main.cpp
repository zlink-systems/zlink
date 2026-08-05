/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_timings.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../common_codecs.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace zlink::samples::deliverydispatch
{

using namespace framework;

class courier_actor_t : public actor_t
{
  public:
    explicit courier_actor_t (actor_context_t context) :
        actor_id (context.actor_ref ().actor_id ().value ()),
        _context (std::move (context))
    {
    }

    actor_context_t &context () noexcept override { return _context; }
    const actor_context_t &context () const noexcept override { return _context; }

    std::string actor_id;
    actor_context_t _context;
    /* 진행 중인 제안: delivery id -> attempt. 배송원의 결정이 오면 이 값을 실어 배차 쪽으로
     * 돌려준다(공통 sample spec §7.4). */
    std::map<std::string, int> offered_attempts;
};

struct courier_actor_factory_t final
    : public actor_factory_t<courier_actor_t>
{
    task_t<std::shared_ptr<courier_actor_t>>
    create (actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<courier_actor_t> (
          std::move (context));
    }
};

class courier_entry_spot_t : public entry_spot_t<courier_actor_t>
{
  public:
    courier_entry_spot_t (entry_spot_context_t context, channel_client_t &channels) :
        _context (std::move (context)), _channels (channels)
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        /* Actor 생성과 위치 조회는 Framework의 ActorManager와 Actor Client가 담당한다.
         * Entry Spot은 actor에 도착한 application message만 처리한다. */
        _context.handlers ()
          .add_actor_request<&courier_entry_spot_t::bind_courier_session> (
            bind_courier_session_req_t::packet_name)
          .add_actor_send<&courier_entry_spot_t::offer_delivery> (
            offer_delivery_msg_t::packet_name)
          .add_actor_send<&courier_entry_spot_t::courier_decision> (
            courier_decision_msg_t::packet_name);
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return spot_actor_join_result_t::accept ();
    }

    task_t<void> on_actor_joined (courier_actor_t &) override { co_return; }
    task_t<void> on_leave_actor (courier_actor_t &) override { co_return; }

    bind_courier_session_res_t bind_courier_session (courier_actor_t &,
                                                    message_context_t &,
                                                    const bind_courier_session_req_t &request)
    {
        return {request.courier_id};
    }

    task_t<void> offer_delivery (courier_actor_t &actor,
                                 message_context_t &,
                                 const offer_delivery_msg_t &message)
    {
        actor.offered_attempts[message.delivery_id] = message.attempt;
        co_await actor.context ()
          .bound_session ()
          .send (offer_delivery_notify_t{message.courier_id, message.delivery_id,
                                         message.pickup_address, message.dropoff_address})
          .submit ();
    }

    /* 배송원의 결정은 배차 쪽으로 one-way로 돌려준다. 노드는 시한을 세지 않는다 — 제안 시한은
     * DispatchWorker가 소유한다(공통 sample spec §7.4). */
    void courier_decision (courier_actor_t &actor,
                           message_context_t &,
                           const courier_decision_msg_t &decision)
    {
        const auto offered = actor.offered_attempts.find (decision.delivery_id);
        if (offered == actor.offered_attempts.end ()) {
            std::cerr << "deliverydispatch courier-actor: decision for an unknown offer delivery="
                      << decision.delivery_id << "\n";
            return;
        }
        const auto attempt = offered->second;
        actor.offered_attempts.erase (offered);

        _channels
          .send (sample_names_t::dispatch_route_channel,
                 offer_delivery_result_msg_t{decision.delivery_id, decision.courier_id, attempt,
                                             decision.accepted, decision.reason})
          .submit ();
    }

  private:
    entry_spot_context_t _context;
    channel_client_t &_channels;
};

} // namespace zlink::samples::deliverydispatch

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::deliverydispatch;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    const std::string instance_name = configuration.role.instance_name;
    if (instance_name.empty ()) {
        throw std::runtime_error (
          "sample.role.instanceName is required for the courier actor node");
    }
    const auto spot_router_endpoint = instance_name == sample_names_t::courier_actor_instance_1
                                        ? topology.courier_actor_node_1_router_endpoint
                                        : topology.courier_actor_node_2_router_endpoint;

    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label ("deliverydispatch-" + instance_name);
        add_deliverydispatch_json_codecs (options.codecs ());
        add_deliverydispatch_location_store (options, topology);
        auto services = options.services ().build_provider ();
        /* 배송원의 결정을 배차 쪽으로 돌려보내는 통로. */
        options.add_client_server_channel (sample_names_t::dispatch_route_channel)
          .client ();
        auto actor_mesh = options.add_route_mesh (sample_names_t::courier_actor_discovery);
        actor_mesh.set_routing_id (zlink::routing_id_t::from (instance_name));
        actor_mesh.set_object_role (object_role_t::server);
        actor_mesh.listen (spot_router_endpoint);
        actor_mesh.channel_name (sample_names_t::courier_actor_discovery).server ();
        actor_mesh.add_entry_spot<courier_entry_spot_t> (
            [services] (entry_spot_context_t context) mutable {
                return std::make_shared<courier_entry_spot_t> (
                  std::move (context), services.get_required<channel_client_t> ());
            })
          .add_actor_factory<courier_actor_t, courier_actor_factory_t> (
            sample_names_t::courier_actor_type,
            std::make_shared<courier_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
    });
    return app.run (argc, argv);
}
