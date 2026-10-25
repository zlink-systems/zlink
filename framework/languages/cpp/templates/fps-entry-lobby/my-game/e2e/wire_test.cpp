#include "server/shared/contracts.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <array>
#include <iostream>
#include <string_view>

int main ()
{
    using namespace MyGame;
    // The .NET public typed serializer owns these cross-language expectations.
    constexpr std::array<std::string_view, 4> golden = {
      R"({"delta":{"x":1,"y":0,"z":-2},"sequence":"7"})",
      R"({"outcome":"hit","targetId":null,"sequence":"9007199254740991"})",
      R"({"roomId":"room-x","nodeId":"matchA","players":[{"playerId":"a","position":{"x":48,"y":0,"z":18},"hp":100,"lastSequence":"4"}],"phase":"Playing","winner":null})",
      R"({"reason":"move-budget","retryAfterMs":120})"};
    zlink::framework::serializer_registry_t serializers;
    // #1694: The pinned C++ default typed serializer emits int64 fields as JSON
    // numbers. MoveRequest, ShotResult and RoomView remain blocked until the
    // Framework serializer supplies the decimal-string contract.
    constexpr std::size_t action_rejected = 3;
    const auto actual = serializers.get<ActionRejected> ()
                          .serialize (ActionRejected{"move-budget", 120}).to_string ();
    if (nlohmann::json::parse (actual) != nlohmann::json::parse (golden[action_rejected])) {
        std::cerr << "Typed wire golden mismatch: ActionRejected" << std::endl;
        return 1;
    }
    std::cout << "Typed wire golden: 1 passed, 3 blocked (#1694)" << std::endl;
}
