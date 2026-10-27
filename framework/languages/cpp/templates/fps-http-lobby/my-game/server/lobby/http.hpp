#pragma once
#include "matchmaker.hpp"
#include "server/shared/http.hpp"
#include "server/shared/http_lobby_contracts.hpp"
#include "server/shared/hosting.hpp"
#include <functional>
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
    Matchmaker matchmaker_;
    static std::string message (std::exception_ptr error)
    {
        try {
            if (error)
                std::rethrow_exception (error);
        }
        catch (const std::exception &failure) {
            return failure.what ();
        }
        catch (...) {
        }
        return "Room preparation failed";
    }
    void watch_waiter (const std::string &mode, const std::string &player,
                       const std::shared_ptr<Connection> &connection)
    {
        connection->stream.expires_never ();
        connection->stream.socket ().async_read_some (net::buffer (&connection->probe, 1),
          [this, mode, player, connection] (auto error, auto) {
              if (!matchmaker_.cancel (mode, player, connection.get ()))
                  return;
              if (!error) {
                  watch_waiter (mode, player, connection);
                  return;
              }
              boost::system::error_code ignored;
              connection->stream.socket ().close (ignored);
          });
    }
    void match (const std::shared_ptr<Connection> &connection)
    {
        if (connection->request.method () != http::verb::post
            || connection->request.target () != LobbyNames::match_path) {
            connection->reply (http::status::not_found, {{"error", "POST /match required"}});
            return;
        }
        MatchRequest request;
        std::optional<Matchmaker::Pairing> pairing;
        try {
            const auto body = nlohmann::json::parse (connection->request.body ());
            request = body.get<MatchRequest> ();
            pairing = matchmaker_.enqueue (
              request, connection.get (),
              [connection] (const MatchResponse &response) {
                  connection->reply (http::status::ok, nlohmann::json (response));
              },
              [connection] (std::exception_ptr error) {
                  connection->reply (http::status::internal_server_error,
                                     {{"error", HttpLobby::message (error)}});
              },
              [connection] {
                  boost::system::error_code ignored;
                  connection->stream.socket ().cancel (ignored);
              });
        }
        catch (const std::exception &error) {
            std::cerr << "HTTP matching failed: " << error.what () << std::endl;
            connection->reply (http::status::bad_request, {{"error", error.what ()}});
            return;
        }
        if (!pairing) {
            watch_waiter (request.mode, request.playerId, connection);
            return;
        }
        pairing->cancel_waiter ();
        try {
            const auto assignment = prepare_ (pairing->roomId, pairing->members);
            if (assignment.roomId != pairing->roomId)
                throw std::invalid_argument ("Prepared Room identity differs from the match.");
            pairing->prepared (assignment);
        }
        catch (...) {
            const auto error = std::current_exception ();
            std::cerr << "HTTP room preparation failed: " << message (error) << std::endl;
            pairing->failed (error);
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
               std::function<MatchResponse (const std::string &,
                                            const std::vector<std::string> &)> prepare) :
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
              routes.request_to_spot (room, PrepareRoom{room, members}).async<RoomPrepared> (),
              Limits::packet);
            if (prepared.matchId != room)
                throw std::invalid_argument ("Prepared Room identity differs from the match.");
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
