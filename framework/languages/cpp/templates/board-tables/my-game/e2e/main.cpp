#include "persist_scenario.hpp"
#include "server_host.hpp"
int main (int argc, char **argv)
{
#ifndef _WIN32
    signal (SIGPIPE, SIG_IGN);
#endif
    std::filesystem::path root = std::filesystem::current_path ();
    std::filesystem::path bin = std::filesystem::absolute (argv[0]).parent_path ();
    std::string selection;
    bool check_only = false, host = false, running = false, attach = false;
    std::unique_ptr<MyGame::E2E::World> world, fresh_world;
    int exit = 0, passed = 0, failed = 0;
    std::vector<MyGame::E2E::ScenarioDefinition> selected;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option (argv[i]);
            if (option == "--check-structure")
                check_only = true;
            else if (option == "--serve")
                host = true;
            else if (option == "--server-running")
                running = true;
            else if (option == "--attach")
                attach = true;
            else if (i + 1 < argc && option == "--root")
                root = argv[++i];
            else if (i + 1 < argc && option == "--scenario")
                selection = argv[++i];
            else
                throw std::invalid_argument ("Usage: my-game-e2e [--root path] [--scenario "
                                             "table,reconnect,match,match-cancel,persist,update] "
                                             "[--attach | --check-structure]");
        }
        MyGame::E2E::require (static_cast<int> (check_only) + host + running + attach <= 1,
                              "Select one runner mode");
        if (running)
            return MyGame::E2E::server_running (root) ? 0 : 1;
        MyGame::E2E::check_structure (root);
        if (host) {
            MyGame::E2E::run_server (root, bin);
            return 0;
        }
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
        if (attach && std::erase_if (selected, [] (const auto &scenario) {
                return scenario.id == MyGame::E2E::Scenario::update;
            }))
            std::cout << "Skipping update: requires isolated node replacement." << std::endl;
        MyGame::E2E::require (!selected.empty (), "Select an attachable scenario");
        const auto domain_checks = MyGame::E2E::domain_checks ();
        std::cout << "Domain checks: " << domain_checks << "/" << MyGame::E2E::domain_check_count
                  << " passed" << std::endl;
        const auto run = [&] (const std::string &endpoint, auto &server) {
            for (const auto &scenario : selected) {
                std::cout << "\n=== " << scenario.name << " ===" << std::endl;
                try {
                    switch (scenario.id) {
                        case MyGame::E2E::Scenario::table:
                            MyGame::E2E::table (endpoint);
                            break;
                        case MyGame::E2E::Scenario::reconnect:
                            fresh_world = std::make_unique<MyGame::E2E::World> (
                              root, bin, 1, false, server.records_location ());
                            MyGame::E2E::reconnect (endpoint, *fresh_world);
                            fresh_world->close ();
                            fresh_world.reset ();
                            break;
                        case MyGame::E2E::Scenario::persist:
                            fresh_world = std::make_unique<MyGame::E2E::World> (
                              root, bin, 1, false, server.records_location ());
                            MyGame::E2E::persist (endpoint, *fresh_world);
                            fresh_world->close ();
                            fresh_world.reset ();
                            break;
                        case MyGame::E2E::Scenario::update:
                            MyGame::E2E::update (endpoint, *world);
                            break;
                        case MyGame::E2E::Scenario::match:
                            MyGame::E2E::play_match (endpoint);
                            break;
                        case MyGame::E2E::Scenario::match_cancel:
                            MyGame::E2E::match_cancel (endpoint);
                            break;
                    }
                    ++passed;
                    std::cout << "PASS " << scenario.name << std::endl;
                }
                catch (const std::exception &error) {
                    ++failed;
                    std::cout << "FAIL " << scenario.name << " - " << error.what () << std::endl;
                    return;
                }
            }
        };
        if (attach) {
            MyGame::E2E::AttachedServer server (root);
            run (server.endpoint (), server);
        } else {
            world =
              std::make_unique<MyGame::E2E::World> (root, bin, static_cast<int> (selected.size ()));
            const auto endpoint = world->start ();
            run (endpoint, *world);
        }
        if (failed)
            exit = 1;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        exit = 1;
    }
    for (auto *owned : {fresh_world.get (), world.get ()}) {
        if (!owned)
            continue;
        try {
            owned->close ();
        }
        catch (const std::exception &error) {
            std::cerr << error.what () << std::endl;
            exit = 1;
        }
    }
    if (!check_only && !host && !running)
        std::cout << "Scenarios: " << passed << "/" << selected.size () << " passed, " << failed
                  << " failed, " << selected.size () - passed - failed << " not run" << std::endl;
    return exit;
}
