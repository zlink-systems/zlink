#include "dungeon.hpp"
#include "server/shared/hosting.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-instance <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        mesh.objects ()
          .server ()
          .add_spot_factory<MyGame::Dungeon, MyGame::NodeConfig> (MyGame::Names::dungeon)
          .recreate_on_relocation ();
        return MyGame::run_host (app, config, true, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
