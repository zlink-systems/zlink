#include "http.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-lobby <config.json>");
        auto config = MyGame::NodeConfig::load (argv[1]);
        if (config.SessionEndpoint.empty () || config.SessionEndpoint.ends_with (":0"))
            throw std::invalid_argument ("Published SessionEndpoint required");
        auto http = std::make_unique<MyGame::HttpLobby> (config);
        config.HttpEndpoint = http->endpoint ();
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        mesh.objects ().client ();
        app.add_hosted_service (std::move (http));
        return MyGame::run_host (app, config, false, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
