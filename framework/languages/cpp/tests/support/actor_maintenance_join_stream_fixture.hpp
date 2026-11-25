/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_host_fixture.hpp"
#include "actor_maintenance_join_packets.hpp"
#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>
#include <stdexcept>
#include <type_traits>

namespace zlink::framework::test::actor_maintenance_join
{
// A Session owner that binds the member Actor on its first packet and relays
// every packet to it, as an application Session does.
class relay_session_t final : public fw::packet_stream_session_t
{
  public:
    relay_session_t (fw::actor_manager_t &actors, std::shared_ptr<observations_t> evidence);
    fw::task_t<void> on_connected (fw::stream_t &) override;
    fw::task_t<void> on_disconnected (fw::stream_t &) override;
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override;
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::framework::message_t &payload) override;

  private:
    fw::actor_manager_t &_actors;
    std::shared_ptr<observations_t> observations;
};

inline constexpr auto session_rid = "join-session";
inline constexpr auto stream_name = "maintenance-join-stream";

template <typename Session = relay_session_t>
void configure_session (host_t &host,
                        const std::shared_ptr<observations_t> &evidence,
                        std::shared_ptr<fw::location_store_t> store,
                        std::shared_ptr<fw::relocation_store_t> relocations)
{
    if constexpr (std::is_same_v<Session, relay_session_t>)
        host.app.advanced ().services ().add_factory<relay_session_t> (
          [evidence] (fw::service_provider_t &provider) {
              return std::make_unique<relay_session_t> (
                provider.get_required<fw::actor_manager_t> (), evidence);
          },
          fw::service_lifetime_t::scoped);
    auto &options = host.app.add_zlink_framework ();
    options.configure_locations ().route_cache_max_age = 0ms;
    options.set_message_follow_duration (source_follow_duration);
    host.app.logging ()
      .use_file (std::string (session_rid) + ".flow")
      .set_min_level (fw::log_level_t::debug);
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (store);
    options.add_relocation_store (relocations);
    options.add_route_mesh (mesh_name)
      .set_object_role (fw::object_role_t::client)
      .set_routing_id (zlink::routing_id_t::from (session_rid))
      .listen ("tcp://127.0.0.1:0");
    options.add_stream_node (stream_name)
      .bind ("tcp://127.0.0.1:0")
      .template register_session<Session> ();
}

extern template void configure_session<relay_session_t> (host_t &,
                                                         const std::shared_ptr<observations_t> &,
                                                         std::shared_ptr<fw::location_store_t>,
                                                         std::shared_ptr<fw::relocation_store_t>);

// One client connection that keeps every received View in arrival order.
class view_client_t
{
  public:
    explicit view_client_t (std::string endpoint);
    ~view_client_t ();
    bool connect ();
    bool wait_closed ();
    std::optional<int> wait_view (std::size_t index);
    std::size_t received_views ();
    // Sends one View request on this connection and returns the View it produced.
    std::optional<int> view ();

  private:
    zlink::stream_connector::connector_t _connector;
    zlink::stream_connector::subscription_t _subscription;
    zlink::stream_connector::subscription_t _closed_subscription;
    bool _closed = false;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<int> _views;
};
} // namespace zlink::framework::test::actor_maintenance_join
