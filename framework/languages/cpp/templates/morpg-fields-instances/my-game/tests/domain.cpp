#include "server/field/domain.hpp"
#include "server/instance/domain.hpp"
#include <iostream>

namespace
{
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
template <class Error, class Action> void rejects (Action action)
{
    try {
        action ();
    }
    catch (const Error &) {
        return;
    }
    throw std::runtime_error ("Invalid input accepted");
}
}

int main ()
{
    using namespace MyGame;
    try {
        require (Names::field_id (199) == "field-2" && Names::field_id (200) == "field-3",
                 "Cross-node boundary differs");
        rejects<std::out_of_range> ([] { Names::field_id (-1); });
        rejects<std::out_of_range> (
          [] { Names::field_id (Names::field_count * Names::field_width); });
        PlayerState player;
        player.apply ({199, 0});
        player.validate ({200, 1});
        rejects<std::invalid_argument> ([&] { player.validate ({200, 2}); });
        player.sequence = std::numeric_limits<int>::max ();
        rejects<std::invalid_argument> ([&] { player.validate ({200, 0}); });

        FieldState field;
        const std::vector<std::string> members{"p1", "p2", "p3", "p4"};
        for (const auto &id : members)
            field.visiblePlayers[id] = {250, 0};
        require (!field.party ("p1", " \t", true), "Blank party accepted");
        require (!field.party ("outsider", "party", true), "Foreign player accepted");
        require (field.party ("p1", "party", true), "Party creation failed");
        require (!field.party ("p1", "other", true), "Duplicate membership accepted");
        require (!field.party ("p2", "missing", false), "Missing party accepted");
        for (std::size_t i = 1; i < members.size (); ++i)
            require (field.party (members[i], "party", false), "Party join failed");
        field.visiblePlayers["p5"] = {250, 0};
        require (!field.party ("p5", "party", false), "Party capacity ignored");
        auto restoredField = nlohmann::json (field).get<FieldState> ();
        require (restoredField.parties.at ("party") == members, "Party snapshot changed order");
        restoredField.leave ("p1");
        require (restoredField.parties.at ("party").front () == "p2", "Leader departure failed");

        DungeonState dungeon;
        const auto id = Names::dungeon_id ("party");
        rejects<std::invalid_argument> ([&] { dungeon.prepare (id, {"party", {"p1", "p1"}}); });
        rejects<std::invalid_argument> ([&] { dungeon.prepare (id, {"party", {" "}}); });
        dungeon.prepare (id, {"party", members});
        require (dungeon.accepts ("p1", id, {id, "party"}), "Prepared member rejected");
        require (!dungeon.accepts ("outsider", id, {id, "party"}), "Foreign member admitted");
        require (!dungeon.accepts ("p1", id, {id, "other"}), "Foreign party admitted");
        rejects<std::invalid_argument> ([&] { dungeon.prepare (id, {"party", members}); });
        dungeon.advance ();
        const auto restored = nlohmann::json (dungeon).get<DungeonState> ();
        require (restored.partyId == "party" && restored.members == members && restored.stage == 1,
                 "Dungeon snapshot lost state");
        dungeon.stage = std::numeric_limits<int>::max ();
        rejects<std::overflow_error> ([&] { dungeon.advance (); });
        std::cout << "Game domain: passed (valid and invalid inputs, snapshot round trips)\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
