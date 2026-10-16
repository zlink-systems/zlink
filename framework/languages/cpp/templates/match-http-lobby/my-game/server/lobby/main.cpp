#include "http.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-lobby <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        if (config.HttpEndpoint.empty ())
            throw std::invalid_argument ("HttpEndpoint required");
        if (config.SessionEndpoint.empty () || config.SessionEndpoint.ends_with (":0"))
            throw std::invalid_argument ("Published SessionEndpoint required");
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        mesh.objects ().client ();
        options.services ().add_singleton<MyGame::Matchmaker> ();
        options.http ()
          .listen (config.HttpEndpoint)
          .map_post<MyGame::MatchHandler> (MyGame::Names::match_path);
        return MyGame::run_host (app, config, false, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
