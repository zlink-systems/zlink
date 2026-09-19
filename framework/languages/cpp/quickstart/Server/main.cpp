#include "../Shared/messages.hpp"

#include <zlink/framework.hpp>

namespace fw = zlink::framework;

// Handles one request on the "greeting" channel.
class hello_handler_t
{
  public:
    using request_type = hello_t;
    using reply_type = greeting_t;

    reply_type handle (const request_type &request) { return greeting_t{"hello, " + request.name}; }
};

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        // Names the mesh and opens this process's endpoint for peers to connect to.
        auto mesh =
          options
            .add_route_mesh ("services")
            .listen ("tcp://0.0.0.0:7301")
            // No Object Server/Client role, so no Location Store is required for
            // this endpoint-only quickstart.
            .set_object_role (fw::object_role_t::none)
            .set_routing_id (zlink::routing_id_t::from ("quickstart-server"))
            .set_advertise_host ("127.0.0.1");
        // This process handles the "greeting" channel.
        mesh
          .channel_name ("greeting")
          .server ()
          .add_request_handler<hello_handler_t, hello_t, greeting_t> ();
    });
    return app.run (argc, argv);
}
