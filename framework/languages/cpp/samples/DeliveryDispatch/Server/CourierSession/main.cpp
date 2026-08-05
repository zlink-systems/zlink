/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../common_codecs.hpp"

#include <zlink/framework.hpp>

#include <iostream>
#include <set>
#include <string>

namespace zlink::samples::deliverydispatch
{

using namespace framework;

class courier_session_t final : public packet_stream_session_t
{
  public:
    task_t<void> on_connected (stream_t &) override { co_return; }

    task_t<void> on_disconnected (stream_t &) override
    {
        _bound_actors.clear ();
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        std::cerr << "deliverydispatch courier-session: dispatch packet="
                  << dispatch.packet_name << "\n";
        auto &actors = stream.actors ();
        if (dispatch.packet_name == bind_courier_session_req_t::packet_name) {
            const auto request = payload.parse_json<bind_courier_session_req_t> ();
            /* Global ActorId로 current owner를 찾거나 eligible node에 생성한다. Application은
             * courier id에서 physical NodeRid를 계산하지 않는다. */
            auto located = actors.get_or_create (
              sample_names_t::courier_actor_type, request.courier_id,
              ensure_courier_actor_req_t{request.courier_id});
            if (!located) {
                throw framework_exception_t (
                  located.error_kind (),
                  located.error () ? located.error ()->what ()
                                   : "courier actor could not be located");
            }
            /* Ready 결과의 exact ActorRef는 Framework session bind에만 사용한다. Application
             * message나 client reply에는 ActorRef와 physical route를 넣지 않는다. */
            auto actor = co_await actors.bind_or_get (located.value ().ref ()).submit ();
            const auto actor_id = std::string (actor.actor_id ());
            _bound_actors.insert (actor_id);
            auto reply = co_await actor
                           .relay_request (bind_courier_session_req_t::packet_name,
                                           zlink::message_t::from_json (bind_courier_session_req_t{
                                             request.courier_id}))
                           .submit ();
            stream.reply_packet (reply).submit ();
            std::cerr << "deliverydispatch courier-session: bound courier=" << request.courier_id
                      << "\n";
            co_return;
        }
        if (dispatch.packet_name == courier_decision_msg_t::packet_name) {
            auto actor =
              require_bound_actor (
                stream, payload.parse_json<courier_decision_msg_t> ().courier_id);
            co_await actor.relay (payload);
            co_return;
        }
    }

  private:
    session_actor_t require_bound_actor (stream_t &stream, const std::string &actor_id)
    {
        if (!_bound_actors.contains (actor_id)) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "courier actor is not bound: " + actor_id);
        }
        auto actor = stream.actors ().find (actor_id);
        if (!actor) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "bound courier actor route is not found: " + actor_id);
        }
        return *actor;
    }

    std::set<std::string> _bound_actors;
};

} // namespace zlink::samples::deliverydispatch

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::deliverydispatch;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label ("deliverydispatch-courier-session");
        add_deliverydispatch_json_codecs (options.codecs ());
        add_deliverydispatch_location_store (options, topology);
        auto actor_mesh = options.add_route_mesh (sample_names_t::courier_actor_discovery);
        actor_mesh.set_routing_id (zlink::routing_id_t::from (
          sample_names_t::courier_session_route_node));
        actor_mesh.set_object_role (object_role_t::client);
        actor_mesh.listen (topology.courier_session_spot_router_endpoint)
          .channel_name (sample_names_t::courier_actor_discovery)
          .client ();
        actor_mesh.peer_connections ().connect (
          zlink::routing_id_t::from (sample_names_t::courier_actor_instance_1),
          topology.courier_actor_node_1_router_endpoint);
        actor_mesh.peer_connections ().connect (
          zlink::routing_id_t::from (sample_names_t::courier_actor_instance_2),
          topology.courier_actor_node_2_router_endpoint);
        options.add_stream_node (sample_names_t::courier_stream_node)
          .bind (topology.courier_stream_endpoint)
          .register_session<courier_session_t> ();
    });
    return app.run (argc, argv);
}
