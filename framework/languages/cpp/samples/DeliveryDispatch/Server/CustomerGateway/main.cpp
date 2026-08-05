/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../common_codecs.hpp"

#include <zlink/framework.hpp>

#include <set>
#include <mutex>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::samples::deliverydispatch
{

using namespace framework;

class customer_session_directory_t
{
  public:
    void subscribe (const std::string &customer_id,
                    const std::string &delivery_id,
                    stream_t stream)
    {
        const std::lock_guard lock (_mutex);
        _subscriptions[delivery_id] = subscription_t{customer_id, std::move (stream)};
        std::cerr << "deliverydispatch customer-session: registered subscription customer="
                  << customer_id << " delivery=" << delivery_id << "\n";
    }

    std::optional<std::string> customer_for_delivery (const std::string &delivery_id) const
    {
        const std::lock_guard lock (_mutex);
        const auto found = _subscriptions.find (delivery_id);
        if (found == _subscriptions.end ()) {
            return std::nullopt;
        }
        return found->second.customer_id;
    }

    void unsubscribe_customer (const std::string &customer_id)
    {
        const std::lock_guard lock (_mutex);
        for (auto it = _subscriptions.begin (); it != _subscriptions.end ();) {
            if (it->second.customer_id == customer_id) {
                it = _subscriptions.erase (it);
            } else {
                ++it;
            }
        }
    }

  private:
    struct subscription_t
    {
        std::string customer_id;
        stream_t stream;
    };

    mutable std::mutex _mutex;
    std::map<std::string, subscription_t> _subscriptions;
};

class customer_actor_t : public actor_t
{
  public:
    explicit customer_actor_t (actor_context_t context) :
        actor_id (context.actor_ref ().actor_id ().value ()),
        _context (std::move (context))
    {
    }

    actor_context_t &context () noexcept override { return _context; }
    const actor_context_t &context () const noexcept override { return _context; }

    std::string actor_id;
    actor_context_t _context;
};

struct customer_actor_factory_t final
    : public actor_factory_t<customer_actor_t>
{
    task_t<std::shared_ptr<customer_actor_t>>
    create (actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<customer_actor_t> (
          std::move (context));
    }
};

class customer_entry_spot_t : public entry_spot_t<customer_actor_t>
{
  public:
    customer_entry_spot_t (entry_spot_context_t context,
                           customer_session_directory_t &sessions) :
        _context (std::move (context)), _sessions (sessions)
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        /* Actor 위치 조회는 Framework의 Actor Directory가 담당한다. Entry Spot은
         * actor에 도착한 subscription과 상태 message만 처리한다. */
        _context.handlers ()
          .add_actor_request<&customer_entry_spot_t::subscribe_delivery> (
            subscribe_delivery_req_t::packet_name)
          .add_actor_send<&customer_entry_spot_t::status_updated> (
            delivery_status_updated_msg_t::packet_name);
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return spot_actor_join_result_t::accept ();
    }

    task_t<void> on_actor_joined (customer_actor_t &) override { co_return; }
    task_t<void> on_leave_actor (customer_actor_t &) override { co_return; }

    subscribe_delivery_res_t
    subscribe_delivery (customer_actor_t &actor,
                        message_context_t &,
                        const subscribe_delivery_req_t &request)
    {
        return {request.delivery_id};
    }

    void status_updated (customer_actor_t &actor,
                         message_context_t &,
                         const delivery_status_updated_msg_t &status)
    {
        auto customer_id = _sessions.customer_for_delivery (status.delivery_id);
        if (!customer_id || *customer_id != actor.actor_id) {
            std::cerr << "deliverydispatch customer-entry: ignored status delivery="
                      << status.delivery_id << " actor=" << actor.actor_id << "\n";
            return;
        }
        std::cerr << "deliverydispatch customer-entry: push status delivery="
                  << status.delivery_id << " status=" << status.status << "\n";
        actor.context ().bound_session ()
          .send (delivery_status_notify_t{status.delivery_id, status.status, status.courier_id,
                                          status.occurred_at_unix_ms})
          .submit ();
    }

  private:
    entry_spot_context_t _context;
    customer_session_directory_t &_sessions;
};

class customer_gateway_session_t final : public packet_stream_session_t
{
  public:
    using dependency_types = dependency_list_t<customer_session_directory_t>;

    explicit customer_gateway_session_t (customer_session_directory_t &sessions) :
        _sessions (sessions)
    {
    }

    task_t<void> on_connected (stream_t &) override { co_return; }

    task_t<void> on_disconnected (stream_t &) override
    {
        for (const auto &actor_id : _bound_actors) {
            _sessions.unsubscribe_customer (actor_id);
        }
        _bound_actors.clear ();
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        std::cerr << "deliverydispatch customer-gateway: dispatch packet="
                  << dispatch.packet_name << "\n";
        if (dispatch.packet_name != subscribe_delivery_req_t::packet_name) {
            auto actor = require_single_bound_actor (
              stream, std::string (dispatch.packet_name));
            if (dispatch.can_reply) {
                auto reply = co_await actor.relay_request (payload).submit ();
                stream.reply_packet (reply).submit ();
                co_return;
            }
            co_await actor.relay (payload);
            co_return;
        }
        const auto request = payload.parse_json<subscribe_delivery_req_t> ();
        auto &actors = stream.actors ();
        auto actor = actors.get_or_create (sample_names_t::customer_actor_type,
                                           sample_names_t::customer_id,
                                           ensure_customer_actor_req_t{
                                             sample_names_t::customer_id});
        if (!actor) {
            throw framework_exception_t (
              actor.error_kind (),
              actor.error () ? actor.error ()->what () : "customer actor create failed");
        }
        auto bound = co_await actors.bind_or_get (actor.value ().ref ()).submit ();
        const auto actor_id = std::string (bound.actor_id ());
        auto current = actors.find (actor_id);
        if (!current) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "bound customer actor route is not found");
        }
        auto reply =
          co_await current->relay_request (zlink::message_t::from_json (request)).submit ();
        _bound_actors.insert (actor_id);
        _sessions.subscribe (actor_id, request.delivery_id, stream);
        stream.reply_packet (reply).submit ();
        std::cerr << "deliverydispatch customer-session: bound customer actor="
                  << actor_id
                  << "\n";
        std::cerr << "deliverydispatch customer-session: subscribed customer="
                  << sample_names_t::customer_id << " delivery=" << request.delivery_id << "\n";
    }

  private:
    session_actor_t require_single_bound_actor (stream_t &stream,
                                                const std::string &packet_name)
    {
        if (_bound_actors.size () != 1) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "single bound customer actor is required for "
                                           + packet_name);
        }
        auto actor = stream.actors ().find (*_bound_actors.begin ());
        if (!actor) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "bound customer actor route is not found for "
                                           + packet_name);
        }
        return *actor;
    }

    customer_session_directory_t &_sessions;
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
          .trace_label ("deliverydispatch-customer-gateway");
        auto sessions = std::make_unique<customer_session_directory_t> ();
        auto *sessions_ptr = sessions.get ();
        options.services ().add_singleton<customer_session_directory_t> (std::move (sessions));
        add_deliverydispatch_json_codecs (options.codecs ());
        add_deliverydispatch_location_store (options, topology);
        auto actor_mesh = options.add_route_mesh (sample_names_t::customer_actor_discovery);
        actor_mesh.set_routing_id (zlink::routing_id_t::from (
          sample_names_t::customer_gateway_route_node));
        actor_mesh.set_object_role (object_role_t::server);
        actor_mesh.listen (topology.customer_spot_router_endpoint);
        actor_mesh.channel_name (sample_names_t::customer_actor_discovery).server ();
        actor_mesh.add_entry_spot<customer_entry_spot_t> ([sessions_ptr] (
                                                            entry_spot_context_t context) {
              return std::make_shared<customer_entry_spot_t> (
                std::move (context), *sessions_ptr);
          })
          .add_actor_factory<customer_actor_t, customer_actor_factory_t> (
            sample_names_t::customer_actor_type,
            std::make_shared<customer_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
        options.add_stream_node (sample_names_t::customer_stream_node)
          .bind (topology.customer_stream_endpoint)
          .register_session<customer_gateway_session_t> ();
    });
    return app.run (argc, argv);
}
