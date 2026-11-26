#include "scenarios.hpp"
#include "server_host.hpp"
void run_domain_checks ();
void run_lobby_domain_checks ();
int main (int argc, char **argv)
{
#ifndef _WIN32
    signal (SIGPIPE, SIG_IGN);
#else
    SetConsoleOutputCP (CP_UTF8);
#endif
    std::filesystem::path root = std::filesystem::current_path ();
    std::filesystem::path bin = std::filesystem::absolute (argv[0]).parent_path ();
    std::string selection;
    bool check_only = false;
    bool serve = false, probe = false, attach = false;
    std::unique_ptr<MyGame::E2E::World> world;
    int exit = 0, passed = 0;
    std::vector<MyGame::E2E::ScenarioDefinition> selected;
    std::string current;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string option (argv[i]);
            if (option == "--serve")
                serve = true;
            else if (option == "--server-running")
                probe = true;
            else if (option == "--attach")
                attach = true;
            else if (option == "--check-structure")
                check_only = true;
            else if (i + 1 < argc && option == "--root")
                root = argv[++i];
            else if (i + 1 < argc && option == "--scenario")
                selection = argv[++i];
            else
                throw std::invalid_argument ("Usage: my-game-e2e [--root path] [--scenario "
                                             "lobby,hit,wall,input,reconnect,update] "
                                             "[--check-structure]");
        }
        if (probe)
            return MyGame::E2E::server_running (root) ? 0 : 1;
        MyGame::E2E::check_structure (root);
        if (serve) {
            MyGame::E2E::run_server (root, bin);
            return 0;
        }
        if (check_only) {
            std::cout << "Structure: passed" << std::endl;
            return 0;
        }
        run_domain_checks ();
        run_lobby_domain_checks ();
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
        const auto server = attach ? MyGame::E2E::running_server (root) : nlohmann::json{};
        if (attach)
            std::erase_if (selected, [] (const auto &scenario) {
                const std::string_view name = scenario.name;
                if (name == "update") {
                    std::cout << "Skipping update: requires isolated node replacement."
                              << std::endl;
                    return true;
                }
                if (name == "lobby") {
                    std::cout << "Skipping lobby: requires restarting the isolated API node."
                              << std::endl;
                    return true;
                }
                return false;
            });
        if (selected.empty ()) {
            std::cout << "Scenarios: 0/0 passed" << std::endl;
            return 0;
        }
        for (const auto &scenario : selected) {
            current = scenario.name;
            std::cout << "━━ " << current << " (" << passed + 1 << '/' << selected.size () << ") ━━"
                      << std::endl;
            world = std::make_unique<MyGame::E2E::World> (
              root, bin, scenario.id == MyGame::E2E::Scenario::lobby ? 2 : 1);
            const auto endpoint = world->start (attach ? &server : nullptr);
            MyGame::E2E::scenario (scenario.id, endpoint, *world);
            world->close ();
            world.reset ();
            ++passed;
            std::cout << "✓ " << scenario.name << std::endl;
        }
    }
    catch (const std::exception &error) {
        if (world)
            MyGame::E2E::log_failure (world->logs (), current, error);
        else
            std::cerr << error.what () << std::endl;
        exit = 1;
    }
    if (world) {
        try {
            world->close ();
        }
        catch (const std::exception &error) {
            MyGame::E2E::log_failure (world->logs (), "Cleanup", error);
            exit = 1;
        }
    }
    if (!check_only && !serve && !probe)
        std::cout << "Scenarios: " << passed << "/" << selected.size () << " passed" << std::endl;
    return exit;
}
