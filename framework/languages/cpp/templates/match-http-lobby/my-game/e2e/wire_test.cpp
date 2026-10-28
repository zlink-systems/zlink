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
    zlink::framework::serializer_registry_t serializers;
    const std::array samples{
      Sample{"JoinRoomRequest", serializers.get<JoinRoomRequest> ().serialize (JoinRoomRequest{"p1", "room-x"}).to_string (),
             nlohmann::json::parse (R"({"playerId":"p1","roomId":"room-x"})")},
      Sample{"MoveRequest", serializers.get<MoveRequest> ().serialize (MoveRequest{1, 0, 7}).to_string (),
             nlohmann::json::parse (R"({"dx":1,"dy":0,"sequence":"7"})")},
      Sample{"ShootRequest", serializers.get<ShootRequest> ().serialize (ShootRequest{"p2", 9'007'199'254'740'993LL}).to_string (),
             nlohmann::json::parse (R"({"targetId":"p2","sequence":"9007199254740993"})")},
      Sample{"MoveResult", serializers.get<MoveResult> ().serialize (MoveResult{41, 10, 7}).to_string (),
             nlohmann::json::parse (R"({"x":41,"y":10,"sequence":"7"})")},
      Sample{"ShotResult", serializers.get<ShotResult> ().serialize (ShotResult{"hit", "p2", 9'007'199'254'740'993LL}).to_string (),
             nlohmann::json::parse (R"({"outcome":"hit","targetId":"p2","sequence":"9007199254740993"})")},
      Sample{"RoomView", serializers.get<RoomView> ().serialize (
               RoomView{"room-x", "matchA", "Playing", std::nullopt,
                        {PlayerView{"p1", 41, 10, 75, 9'007'199'254'740'993LL}}}).to_string (),
             nlohmann::json::parse (R"({"roomId":"room-x","nodeId":"matchA","phase":"Playing","winner":null,"players":[{"playerId":"p1","x":41,"y":10,"hp":75,"lastSequence":"9007199254740993"}]})")},
      Sample{"ActionRejected", serializers.get<ActionRejected> ().serialize (ActionRejected{"move-budget", 120}).to_string (),
             nlohmann::json::parse (R"({"reason":"move-budget","retryAfterMs":120})")},
      Sample{"MatchEnded", serializers.get<MatchEnded> ().serialize (MatchEnded{"p1"}).to_string (),
             nlohmann::json::parse (R"({"winner":"p1"})")}};
    for (const auto &sample : samples) {
        if (nlohmann::json::parse (sample.actual) != sample.expected) {
            std::cerr << "Wire golden mismatch: " << sample.name << ": " << sample.actual << std::endl;
            return 1;
        }
    }
    std::cout << "Wire golden: 8/8 passed" << std::endl;
}
