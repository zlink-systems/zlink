#include "pool.hpp"
#include "server/shared/hosting.hpp"

int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-matchmaker <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        mesh.objects ()
          .server ()
          .add_spot_factory<MyGame::Pool, MyGame::fx::spot_manager_t, MyGame::fx::actor_client_t> (
            MyGame::MatchNames::pool)
          .set_stable_type_limit (1)
          .set_execution_mode (MyGame::fx::user_spot_execution_mode_t::spot_wide)
          .disable_relocation ();
        return MyGame::run_host (app, config, true, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
