#include "server/shared/contracts.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <array>
#include <iostream>
#include <string_view>

int main ()
{
    using namespace MyGame;
    struct Sample
    {
        std::string_view name;
        std::string actual;
        nlohmann::json expected = nlohmann::json::object ();
    };
    constexpr std::array<std::string_view, 4> golden = {
      R"({"delta":{"x":1,"y":0,"z":-2},"sequence":"7"})",
      R"({"outcome":"hit","targetId":null,"sequence":"9007199254740991"})",
      R"({"roomId":"room-x","nodeId":"matchA","players":[{"playerId":"a","position":{"x":48,"y":0,"z":18},"hp":100,"lastSequence":"4"}],"phase":"Playing","winner":null})",
      R"({"reason":"move-budget","retryAfterMs":120})"};
    zlink::framework::serializer_registry_t serializers;
    const std::array samples{
      Sample{"MoveRequest", serializers.get<MoveRequest> ().serialize (MoveRequest{Vec3{1, 0, -2}, 7}).to_string (),
             nlohmann::json::parse (golden[0])},
      Sample{"ShotResult", serializers.get<ShotResult> ().serialize (ShotResult{"hit", std::nullopt, 9'007'199'254'740'991LL}).to_string (),
             nlohmann::json::parse (golden[1])},
      Sample{"RoomView", serializers.get<RoomView> ().serialize (
               RoomView{"room-x", "matchA", {PlayerView{"a", Vec3{48, 0, 18}, 100, 4}}, MatchPhase::playing, std::nullopt}).to_string (),
             nlohmann::json::parse (golden[2])},
      Sample{"ActionRejected", serializers.get<ActionRejected> ().serialize (ActionRejected{"move-budget", 120}).to_string (),
             nlohmann::json::parse (golden[3])}};
    for (const auto &sample : samples) {
        if (nlohmann::json::parse (sample.actual) != sample.expected) {
            std::cerr << "Typed wire golden mismatch: " << sample.name << ": " << sample.actual << std::endl;
            return 1;
        }
    }
    std::cout << "Typed wire golden: 4/4 passed" << std::endl;
}
