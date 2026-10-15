#include "dungeon.hpp"
#include "server/shared/hosting.hpp"
int
main(int argc, char** argv)
{
  try {
    if (argc != 2)
      throw std::invalid_argument("Usage: my-game-instance <config.json>");
    const auto config = MyGame::NodeConfig::load(argv[1]);
    if (config.DungeonCapacity <= 0)
      throw std::invalid_argument("DungeonCapacity required");
    auto app = MyGame::fx::app_t::create();
    auto& options = MyGame::configure_host(app, config);
    auto mesh = options.add_route_mesh(MyGame::Names::mesh);
    mesh.set_automatic_routing_id_prefix(config.Id).listen(config.MeshEndpoint);
    auto objects = mesh.objects().server();
    mesh.add_actor_factory<MyGame::Player, MyGame::PlayerFactory>(
      MyGame::Names::player,
      std::make_shared<MyGame::PlayerFactory>(),
      [](auto& factory) { factory.recreate_on_relocation(); });
    objects
      .add_spot_factory<MyGame::Dungeon,
                        MyGame::NodeConfig,
                        MyGame::fx::actor_client_t>(MyGame::Names::dungeon)
      .set_stable_type_limit(config.DungeonCapacity)
      .set_execution_mode(MyGame::fx::user_spot_execution_mode_t::spot_wide)
      .preserve_state_with<MyGame::DungeonRelocation>();
    return MyGame::run_host(app, config, true, argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
