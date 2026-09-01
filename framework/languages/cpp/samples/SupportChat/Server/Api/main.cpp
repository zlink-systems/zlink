/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/* SupportChat API 서버.
 *
 * 공통 sample spec: 인증(`AuthenticateUserReq/Res`)과 대화 개설 접수
 * (`OpenConversationApiReq/Res`)는 API 서버가 소유한다. API는 SpotManager로 conversation
 * User Spot을 만들고, Session 서버는 stream 경계만 다룬다. */

#include "../../Shared/Contracts/messages.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_readiness.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <string>

namespace zlink::samples::supportchat
{
namespace
{

using namespace zlink::framework;

struct supportchat_user_t
{
    std::string actor_id;
    std::string display_name;
    std::string role;
};

/* 샘플 사용자 디렉터리: access token -> 사용자 프로필. */
class user_directory_t
{
  public:
    std::optional<supportchat_user_t> find (const std::string &access_token) const
    {
        const auto found = _users.find (access_token);
        if (found == _users.end ()) {
            return std::nullopt;
        }
        return found->second;
    }

  private:
    std::map<std::string, supportchat_user_t> _users{
      {"agent", supportchat_user_t{"agent-1", "Agent One", std::string (role_t::agent)}},
      {"customer",
       supportchat_user_t{"customer-1", "Customer One", std::string (role_t::customer)}},
      {"customer-2",
       supportchat_user_t{"customer-2", "Customer Two", std::string (role_t::customer)}},
      {"customer-3",
       supportchat_user_t{"customer-3", "Customer Three", std::string (role_t::customer)}}};
};

class authenticate_user_handler_t
{
  public:
    using request_type = authenticate_user_req_t;
    using reply_type = authenticate_user_res_t;
    using dependency_types = dependency_list_t<user_directory_t>;
    static constexpr const char *topic_name = authenticate_user_req_t::packet_name;

    explicit authenticate_user_handler_t (user_directory_t &users) : _users (users) {}

    authenticate_user_res_t handle (const authenticate_user_req_t &request)
    {
        const auto user = _users.find (request.access_token);
        if (!user) {
            std::cerr << "supportchat api: authenticate rejected token=" << request.access_token
                      << "\n";
            return authenticate_user_res_t{false, std::nullopt, std::nullopt, std::nullopt,
                                           std::string ("UnknownAccessToken")};
        }
        std::cerr << "supportchat api: authenticate actor=" << user->actor_id << "\n";
        return authenticate_user_res_t{true, user->actor_id, user->display_name, user->role,
                                       std::nullopt};
    }

  private:
    user_directory_t &_users;
};

class open_conversation_api_handler_t
{
  public:
    using request_type = open_conversation_api_req_t;
    using reply_type = open_conversation_api_res_t;
    using dependency_types = dependency_list_t<spot_manager_t>;
    static constexpr const char *topic_name = open_conversation_api_req_t::packet_name;

    explicit open_conversation_api_handler_t (spot_manager_t &spots) : _spots (spots) {}

    task_t<open_conversation_api_res_t> handle (const open_conversation_api_req_t &request)
    {
        auto created =
          co_await _spots.create (sample_names_t::conversation_spot)
            .in_mesh (sample_names_t::mesh)
            .creation_request (conversation_create_req_t{
              request.customer_actor_id, request.customer_display_name, request.subject,
              std::chrono::duration_cast<std::chrono::milliseconds> (
                std::chrono::system_clock::now ().time_since_epoch ())
                .count ()})
            .async ();
        if (created.state == spot_create_state_t::rejected || !created.reply) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "SupportChat conversation creation returned no state");
        }
        const auto response = created.reply->decode<conversation_create_res_t> ();
        std::cerr << "supportchat api: conversation created id=" << response.state.conversation_id
                  << " status=" << response.state.status << "\n";
        co_return open_conversation_api_res_t{response.state};
    }

  private:
    spot_manager_t &_spots;
};

} // namespace
} // namespace zlink::samples::supportchat

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::supportchat;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    app.logging ().use_file (configuration.flow_log_path ());
    auto &options = app.add_zlink_framework ();
    options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
    options.add_location_store<redis::redis_location_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "location:");
    options.add_relocation_store<redis::redis_relocation_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "relocation:");
    options.services ().add_singleton<user_directory_t> ();

    options.add_client_server_channel ("supportchat.api")
      .server ()
      .set_bind_host (host_from_tcp_endpoint (topology.api_route_endpoint))
      .set_advertise_host (host_from_tcp_endpoint (topology.api_route_endpoint))
      .listen (port_from_tcp_endpoint (topology.api_route_endpoint))
      .add_handler_group ("supportchat-api");
    auto support_spot = options.add_route_mesh (sample_names_t::mesh);
    support_spot.set_routing_id (zlink::routing_id_t::from ("supportchat-api"))
      .listen (topology.api_spot_route_endpoint);
    support_spot.objects ().client ();

    options.handlers ()
      .group ("supportchat-api")
      .add<authenticate_user_handler_t> ()
      .add<open_conversation_api_handler_t> ();
    app.add_hosted_service (
      std::make_unique<sample_readiness_service_t> ("public", "api"));
    app.add_hosted_service (std::make_unique<spot_route_readiness_service_t> (
      sample_names_t::mesh, "api"));
    return app.run (argc, argv);
}
