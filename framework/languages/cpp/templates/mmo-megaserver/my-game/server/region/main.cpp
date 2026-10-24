#include "infrastructure/zlink/region.hpp"
#include "server/shared/hosting.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-region <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        auto objects = mesh.objects ().server ();
        objects.add_entry_spot<MyGame::Entry, MyGame::fx::spot_manager_t> ();
        mesh.add_actor_factory<MyGame::Player, MyGame::PlayerFactory> (
          MyGame::Names::player,
          std::make_shared<MyGame::PlayerFactory> (options.services ().build_provider ()),
          [] (auto &factory) {
              factory.template preserve_state_with<MyGame::PlayerRelocation> ();
          });
        objects
          .add_spot_factory<MyGame::Region, MyGame::NodeConfig, MyGame::fx::actor_client_t,
                            MyGame::fx::spot_manager_t> (MyGame::Names::region)
          .set_stable_type_limit (config.RegionLimit)
          .set_execution_mode (MyGame::fx::user_spot_execution_mode_t::spot_wide)
          .preserve_state_with<MyGame::RegionRelocation> ();
        return MyGame::run_host (app, config, true, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
