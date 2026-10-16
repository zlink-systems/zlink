#include "session.hpp"
#include "server/shared/hosting.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-session <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        if (!config.StreamEndpoint)
            throw std::invalid_argument ("Session StreamEndpoint required");
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh (config.WorldId));
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        mesh.objects ().client ();
        options.add_stream_node (MyGame::Names::stream)
          .bind (*config.StreamEndpoint)
          .register_session<MyGame::Session> ();
        return MyGame::run_host (app, config, false, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
