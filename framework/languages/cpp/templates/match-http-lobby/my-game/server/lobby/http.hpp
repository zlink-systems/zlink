#pragma once
#include "server/shared/http.hpp"
#include "server/shared/hosting.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
namespace MyGame
{
// All lobby queue and HTTP socket state belongs to this single IO executor.
class HttpLobby final : public fx::hosted_service_t
{
    struct Connection : std::enable_shared_from_this<Connection>
    {
        boost::beast::tcp_stream stream;
        boost::beast::flat_buffer buffer;
        char probe;
        http::request<http::string_body> request;
        http::response<http::string_body> response;
        explicit Connection (Tcp::socket socket) : stream (std::move (socket)) {}
        void reply (http::status status, const nlohmann::json &body)
        {
            response = {status, request.version ()};
            response.set (http::field::content_type, "application/json");
            response.keep_alive (false);
            response.body () = body.dump ();
            response.prepare_payload ();
            stream.expires_after (Limits::packet);
            http::async_write (stream, response, [self = shared_from_this ()] (auto error, auto) {
                boost::system::error_code ignored;
                self->stream.socket ().close (ignored);
                if (error)
                    std::cerr << "HTTP response failed: " << error.message () << std::endl;
            });
        }
    };
    net::io_context io_;
    Tcp::acceptor acceptor_;
    std::thread worker_;
    const NodeConfig &config_;
    std::function<MatchResponse (const std::string &, const std::vector<std::string> &)> prepare_;
    struct Waiting
    {
        std::string player;
        std::shared_ptr<Connection> connection;
    };
    std::map<std::string, Waiting> queues_;
    std::set<std::string> participants_;
    void watch_waiter (const std::string &mode, const std::shared_ptr<Connection> &connection)
    {
        connection->stream.expires_never ();
        connection->stream.socket ().async_read_some (
          net::buffer (&connection->probe, 1), [this, mode, connection] (auto error, auto) {
              const auto waiting = queues_.find (mode);
              if (waiting == queues_.end () || waiting->second.connection != connection)
                  return;
              if (!error) {
                  watch_waiter (mode, connection);
                  return;
              }
              participants_.erase (waiting->second.player);
              queues_.erase (waiting);
              boost::system::error_code ignored;
              connection->stream.socket ().close (ignored);
          });
    }
    void match (const std::shared_ptr<Connection> &connection)
    {
        if (connection->request.method () != http::verb::post
            || connection->request.target () != Names::match_path) {
            connection->reply (http::status::not_found, {{"error", "POST /match required"}});
            return;
        }
        std::shared_ptr<Connection> partner;
        std::string player;
        try {
            const auto body = nlohmann::json::parse (connection->request.body ());
            const MatchRequest request{body.at ("playerId").get<std::string> (),
                                       body.value ("mode", std::string (Names::default_mode))};
            if (request.playerId.find_first_not_of (" \t\r\n") == std::string::npos
                || request.mode.find_first_not_of (" \t\r\n") == std::string::npos
                || participants_.contains (request.playerId))
                throw std::invalid_argument (
                  "Nonempty player/mode and distinct pending player required");
            player = request.playerId;
            const auto found = queues_.find (request.mode);
            if (found == queues_.end ()) {
                participants_.insert (player);
                queues_.emplace (request.mode, Waiting{player, connection});
                watch_waiter (request.mode, connection);
                return;
            }
            partner = found->second.connection;
            boost::system::error_code ignored;
            partner->stream.socket ().cancel (ignored);
            const auto partner_player = found->second.player;
            participants_.erase (found->second.player);
            queues_.erase (found);
            auto room_uuid = boost::uuids::to_string (boost::uuids::random_generator{} ());
            room_uuid.erase (std::remove (room_uuid.begin (), room_uuid.end (), '-'),
                             room_uuid.end ());
            const auto room = std::string (Names::room_prefix) + room_uuid;
            const std::vector<std::string> members{partner_player, player};
            const nlohmann::json assignment = prepare_ (room, members);
            partner->reply (http::status::ok, assignment);
            connection->reply (http::status::ok, assignment);
        }
        catch (const std::exception &error) {
            std::cerr << "HTTP matching failed: " << error.what () << std::endl;
            const nlohmann::json body{{"error", error.what ()}};
            if (partner)
                partner->reply (http::status::internal_server_error, body);
            connection->reply (
              partner ? http::status::internal_server_error : http::status::bad_request, body);
        }
    }
    void accept ()
    {
        acceptor_.async_accept ([this] (auto error, Tcp::socket socket) {
            if (error) {
                if (error != net::error::operation_aborted)
                    std::cerr << "HTTP accept failed: " << error.message () << std::endl;
                return;
            }
            auto connection = std::make_shared<Connection> (std::move (socket));
            connection->stream.expires_after (Limits::packet);
            http::async_read (connection->stream, connection->buffer, connection->request,
                              [this, connection] (auto error, auto) {
                                  if (error) {
                                      std::cerr << "HTTP request failed: " << error.message ()
                                                << std::endl;
                                      return;
                                  }
                                  match (connection);
                              });
            accept ();
        });
    }

    void start_io ()
    {
        accept ();
        worker_ = std::thread ([this] { io_.run (); });
    }

  public:
    explicit HttpLobby (const NodeConfig &config) :
        acceptor_ (io_, http_address (config.HttpEndpoint)), config_ (config)
    {
    }
    HttpLobby (const NodeConfig &config,
               std::function<MatchResponse (const std::string &, const std::vector<std::string> &)> prepare) :
        HttpLobby (config)
    {
        prepare_ = std::move (prepare);
        start_io ();
    }
    std::string endpoint () const
    {
        const auto local = acceptor_.local_endpoint ();
        return "http://" + local.address ().to_string () + ":" + std::to_string (local.port ());
    }
    fx::task_t<void> start (fx::service_provider_t &services) override
    {
        auto &spots = services.get_required<fx::spot_manager_t> ();
        auto &routes = services.get_required<fx::route_client_t> ();
        prepare_ = [this, &spots, &routes] (const std::string &room,
                                            const std::vector<std::string> &members) {
            const auto created =
              await_host (spots.get_or_create (room, Names::room).in_mesh (Names::mesh).async (),
                          Limits::packet);
            if (created.state == fx::spot_create_state_t::rejected)
                throw std::runtime_error ("Room preparation rejected");
            const auto prepared = await_host (
              routes.request_to_spot (room, PrepareRoom{room, members})
                .async<RoomPrepared> (),
              Limits::packet);
            if (prepared.matchId != room)
                throw std::runtime_error ("Prepared Room identity differs from the match");
            return MatchResponse{room, config_.SessionEndpoint};
        };
        start_io ();
        co_return;
    }
    void stop () noexcept override
    {
        io_.stop ();
        if (worker_.joinable ())
            worker_.join ();
    }
    ~HttpLobby () override { stop (); }
};
}
