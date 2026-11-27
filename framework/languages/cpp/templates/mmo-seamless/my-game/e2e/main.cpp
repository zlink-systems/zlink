#include "scenarios.hpp"
#include "server_host.hpp"
#include "domain_checks.hpp"
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
    bool check_only = false, check_domain_only = false;
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
            else if (option == "--check-domain")
                check_domain_only = true;
            else if (i + 1 < argc && option == "--root")
                root = argv[++i];
            else if (i + 1 < argc && option == "--scenario")
                selection = argv[++i];
            else
                throw std::invalid_argument (
                  "Usage: my-game-e2e [--root path] [--scenario "
                  "cross,border,reconnect,update] [--check-structure] [--check-domain]");
        }
        if (probe)
            return MyGame::E2E::server_running (root) ? 0 : 1;
        MyGame::E2E::check_structure (root);
        if (serve) {
            MyGame::E2E::run_server (root, bin);
            return 0;
        }
        if (check_only && !check_domain_only) {
            std::cout << "Structure: passed" << std::endl;
            return 0;
        }
        MyGame::E2E::run_domain_checks ();
        if (check_domain_only)
            return 0;
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
                return false;
            });
        if (selected.empty ()) {
            std::cout << "Scenarios: 0/0 passed" << std::endl;
            return 0;
        }
        world = std::make_unique<MyGame::E2E::World> (root, bin);
        const auto endpoint = world->start (attach ? &server : nullptr);
        for (const auto &scenario : selected) {
            current = scenario.name;
            std::cout << "━━ " << current << " (" << passed + 1 << '/' << selected.size () << ") ━━"
                      << std::endl;
            switch (scenario.id) {
                case MyGame::E2E::Scenario::cross:
                    MyGame::E2E::cross (endpoint);
                    break;
                case MyGame::E2E::Scenario::border:
                    MyGame::E2E::border (endpoint);
                    break;
                case MyGame::E2E::Scenario::reconnect:
                    MyGame::E2E::reconnect (endpoint);
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
