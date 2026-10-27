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
        nlohmann::json expected;
    };
    zlink::framework::serializer_registry_t serializers;
    const std::array samples{
      Sample{"QueueRequest", serializers.get<QueueRequest> ().serialize (QueueRequest{"p1", "duel"}).to_string (),
             nlohmann::json::parse (R"({"playerId":"p1","mode":"duel"})")},
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
    // #1694: Exclude MoveRequest, ShootRequest, MoveResult, ShotResult and RoomView because they contain int64 fields.
    std::cout << "Wire golden: 3/3 common samples passed; int64 samples excluded (#1694)" << std::endl;
}
