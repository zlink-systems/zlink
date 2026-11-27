#include "entry.hpp"
#include "room_provisioner.hpp"
#include "table.hpp"
#include "server/shared/hosting.hpp"
int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-table <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        options.add_client_server_channel (MyGame::player_records_channel).client ();
        auto services = std::make_unique<MyGame::PlayerServices> ();
        auto player_factory = std::make_shared<MyGame::PlayerFactory> (*services);
        app.add_hosted_service (std::move (services));
        auto mesh = options.add_route_mesh (MyGame::Names::mesh);
        mesh.set_automatic_routing_id_prefix (config.Id).listen (config.MeshEndpoint);
        auto objects = mesh.objects ().server ();
        objects.add_entry_spot<MyGame::Entry, MyGame::fx::spot_manager_t> ();
        objects
          .add_instance_spot_factory<MyGame::RoomProvisioner, MyGame::fx::spot_manager_t> (
            MyGame::MatchNames::provisioner)
          .recreate_on_relocation ();
        mesh.add_actor_factory<MyGame::Player, MyGame::PlayerFactory> (
          MyGame::Names::player, player_factory, [] (auto &factory) {
              factory.template preserve_state_with<MyGame::PlayerRelocation> ();
          });
        auto table_factory =
          objects.add_spot_factory<MyGame::Table, MyGame::fx::actor_client_t,
                                   MyGame::fx::route_client_t, MyGame::NodeConfig> (
            MyGame::Names::table);
        if (config.TableCapacity > 0)
            table_factory.set_stable_type_limit (config.TableCapacity);
        table_factory.set_execution_mode (MyGame::fx::user_spot_execution_mode_t::spot_wide)
          .preserve_state_with<MyGame::TableRelocation> ();
        return MyGame::run_host (app, config, true, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
