#include "scenarios.hpp"
int main (int argc, char **argv)
{
#ifndef _WIN32
    signal (SIGPIPE, SIG_IGN);
#endif
    std::filesystem::path root = std::filesystem::current_path ();
    std::filesystem::path bin = std::filesystem::absolute (argv[0]).parent_path ();
    std::string selection;
    bool check_only = false;
    std::unique_ptr<MyGame::E2E::World> world;
    int exit = 0, passed = 0;
    std::vector<MyGame::E2E::ScenarioDefinition> selected;
    std::string current;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option (argv[i]);
            if (option == "--check-structure")
                check_only = true;
            else if (i + 1 < argc && option == "--root")
                root = argv[++i];
            else if (i + 1 < argc && option == "--scenario")
                selection = argv[++i];
            else
                throw std::invalid_argument ("Usage: my-game-e2e [--root path] [--scenario "
                                             "lobby,hit,reconnect,update] [--check-structure]");
        }
        MyGame::E2E::check_structure (root);
        if (check_only) {
            std::cout << "Structure: passed" << std::endl;
            return 0;
        }
        if (selection.empty ())
            selected.assign (MyGame::E2E::scenario_order.begin (),
                             MyGame::E2E::scenario_order.end ());
        else {
            std::set<std::string> requested;
            std::istringstream input (selection);
            std::string name;
            while (std::getline (input, name, ',')) {
                MyGame::E2E::require (
                  std::find_if (MyGame::E2E::scenario_order.begin (),
                                MyGame::E2E::scenario_order.end (),
                                [&] (const auto &scenario) { return name == scenario.name; })
                      != MyGame::E2E::scenario_order.end ()
                    && requested.insert (name).second,
                  "Select distinct known scenarios");
            }
            MyGame::E2E::require (!selection.ends_with (',') && !requested.empty (),
                                  "Empty scenario");
            for (const auto &scenario : MyGame::E2E::scenario_order)
                if (requested.contains (scenario.name))
                    selected.push_back (scenario);
        }
        world = std::make_unique<MyGame::E2E::World> (root, bin);
        const auto endpoint = world->start ();
        for (const auto &scenario : selected) {
            current = scenario.name;
            switch (scenario.id) {
                case MyGame::E2E::Scenario::lobby:
                    MyGame::E2E::lobby (endpoint, *world);
                    break;
                case MyGame::E2E::Scenario::hit:
                    MyGame::E2E::hit (endpoint, *world);
                    break;
                case MyGame::E2E::Scenario::reconnect:
                    MyGame::E2E::reconnect (endpoint, *world);
                    break;
                case MyGame::E2E::Scenario::update:
                    MyGame::E2E::update (endpoint, *world);
                    break;
            }
            ++passed;
            std::cout << "✓ " << scenario.name << std::endl;
        }
    }
    catch (const std::exception &error) {
        if (!current.empty ())
            std::cout << "✗ " << current << std::endl;
        std::cerr << error.what () << std::endl;
        exit = 1;
    }
    if (world) {
        try {
            world->close ();
        }
        catch (const std::exception &error) {
            std::cerr << error.what () << std::endl;
            exit = 1;
        }
    }
    if (!check_only)
        std::cout << "Scenarios: " << passed << "/" << selected.size () << " passed" << std::endl;
    return exit;
}
