#include "server/shared/config.hpp"
#include "server/shared/http.hpp"
#include <iostream>
#include <thread>

namespace MyGame
{
class HttpRequest final : public std::enable_shared_from_this<HttpRequest>
{
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    http::request<http::empty_body> request_;
    http::response<http::string_body> response_;
    const std::string &worlds_;

  public:
    HttpRequest (Tcp::socket socket, const std::string &worlds) :
        stream_ (std::move (socket)), worlds_ (worlds)
    {
    }
    void start ()
    {
        stream_.expires_after (Limits::packet);
        http::async_read (
          stream_, buffer_, request_, [self = shared_from_this ()] (auto error, auto) {
              if (error) {
                  std::cerr << "HTTP read failed: " << error.message () << std::endl;
                  return;
              }
              const bool found = self->request_.method () == http::verb::get
                                 && self->request_.target () == Names::worlds_path;
              self->response_ = {found ? http::status::ok : http::status::not_found,
                                 self->request_.version ()};
              self->response_.set (http::field::content_type, "application/json");
              self->response_.body () = found ? self->worlds_ : "{}";
              self->response_.keep_alive (false);
              self->response_.prepare_payload ();
              http::async_write (self->stream_, self->response_, [self] (auto write_error, auto) {
                  if (write_error)
                      std::cerr << "HTTP write failed: " << write_error.message () << std::endl;
              });
          });
    }
};
class Accounts
{
    Tcp::acceptor acceptor_;
    const std::string worlds_;

  public:
    Accounts (net::io_context &io,
              const std::string &endpoint,
              const std::vector<WorldInfo> &worlds) :
        acceptor_ (io), worlds_ (nlohmann::json (worlds).dump ())
    {
        const auto [host, port] = http_address (endpoint);
        Tcp::resolver resolver (io);
        const auto address = *resolver.resolve (host, port).begin ();
        acceptor_.open (address.endpoint ().protocol ());
        acceptor_.bind (address.endpoint ());
        acceptor_.listen ();
        accept ();
    }
    std::string endpoint () const
    {
        const auto actual = acceptor_.local_endpoint ();
        return "http://" + actual.address ().to_string () + ":" + std::to_string (actual.port ());
    }
    void accept ()
    {
        acceptor_.async_accept ([this] (auto error, auto socket) {
            if (error)
                throw std::runtime_error ("HTTP accept failed: " + error.message ());
            std::make_shared<HttpRequest> (std::move (socket), worlds_)->start ();
            accept ();
        });
    }
};
}
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-accounts <config.json>");
        const auto config = MyGame::read_json (argv[1]);
        const auto worlds = config.at ("Worlds").get<std::vector<MyGame::WorldInfo>> ();
        if (worlds.empty ())
            throw std::invalid_argument ("Accounts requires worlds");
        MyGame::net::io_context io;
        MyGame::Accounts accounts (io, config.at ("HttpEndpoint"), worlds);
        std::exception_ptr failure;
        std::jthread worker ([&] {
            try {
                io.run ();
            }
            catch (...) {
                failure = std::current_exception ();
            }
        });
        std::cout
          << MyGame::Control::ready
          << nlohmann::json{{"id", config.at ("Id")}, {"endpoint", accounts.endpoint ()}}.dump ()
          << std::endl;
        std::string command;
        const bool shutdown =
          std::getline (std::cin, command) && command == MyGame::Control::shutdown;
        io.stop ();
        worker.join ();
        if (failure)
            std::rethrow_exception (failure);
        if (!shutdown)
            throw std::runtime_error ("Accounts requires shutdown command");
        std::cout << MyGame::Control::stopped << std::endl;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
