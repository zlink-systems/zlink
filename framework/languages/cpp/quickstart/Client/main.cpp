#include "../Shared/messages.hpp"

#include <zlink/framework.hpp>

namespace fw = zlink::framework;

// Serves GET /hello/{name}: calls "greeting" on the mesh and returns the reply.
class hello_http_handler_t
{
  public:
    explicit hello_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto name = request.route_values.at ("name");
        auto reply = co_await _routes.request_to_channel ("greeting", hello_t{name})
                       .template async<greeting_t> ();
        co_return fw::http_response_t{200, "\"" + reply.text + "\"", "application/json"};
    }

  private:
    fw::route_client_t &_routes;
};

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        // This process also needs its own endpoint.
        auto mesh = options.add_route_mesh ("services")
                      .listen ("tcp://127.0.0.1:7302")
                      .set_object_role (fw::object_role_t::none)
                      .set_routing_id (zlink::routing_id_t::from ("quickstart-client"))
                      .set_advertise_host ("127.0.0.1");
        // This side only calls; it does not handle "greeting".
        mesh.channel_name ("greeting").client ();
        // Manual connection -- the server's endpoint is given directly.
        mesh.peer_connections ().connect ("tcp://127.0.0.1:7301");

        options.http ()
          .listen ("http://127.0.0.1:5083")
          .map_get<hello_http_handler_t> ("/hello/{name}");
    });
    return app.run (argc, argv);
}
