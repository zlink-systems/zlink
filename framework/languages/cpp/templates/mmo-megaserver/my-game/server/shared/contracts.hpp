#pragma once
#include <zlink/json.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-world";
    static constexpr auto region = "Region";
    static constexpr auto player = "Player";
    static constexpr auto dungeon = "Dungeon";
    static constexpr auto stream = "players";
    static constexpr auto region_full = "region-full";
    static constexpr int max_replicas_per_region = 2;
    static constexpr int max_step = 10;
    static constexpr int region_min_x = 0;
    static constexpr int region_max_x = 100;
    static constexpr double max_speed = max_step * 5.0;
    static constexpr std::int64_t max_safe_integer = 9'007'199'254'740'991LL;
    static constexpr std::array<std::string_view, 2> allowed_regions{"forest", "desert"};
    static constexpr int first_index = 1;
    static constexpr int default_capacity = 2;
    static constexpr int default_region_limit = 2;
    static std::string region_id (const std::string &name, int index)
    {
        if (name.find_first_not_of (" \t\r\n") == std::string::npos || index < first_index)
            throw std::invalid_argument ("Invalid region selection");
        return name + "-" + std::to_string (index);
    }
    static bool is_allowed_region (std::string_view name)
    {
        return std::find (allowed_regions.begin (), allowed_regions.end (), name)
               != allowed_regions.end ();
    }
};
struct Rejection
{
    static constexpr auto packet_not_allowed = "packet-not-allowed";
    static constexpr auto non_finite = "non-finite";
    static constexpr auto step_too_large = "step-too-large";
    static constexpr auto outside_region = "outside-region";
    static constexpr auto move_budget = "move-budget";
    static constexpr auto stale_sequence = "stale-sequence";
    static constexpr auto region_unknown = "region-unknown";
    static constexpr auto capacity_exhausted = "capacity-exhausted";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
};
struct Limits
{
    static constexpr auto isolation = std::chrono::milliseconds (300);
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe_observation = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
struct Control
{
    static constexpr auto ready = "READY ";
    static constexpr auto drain = "drain";
    static constexpr auto shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED";
    static constexpr auto relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN";
    static constexpr auto stopped = "STOPPED";
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string SessionId, ActorId;
    std::uint64_t ObjectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;

};
struct EnterRegionRequest
{
    static constexpr auto packet_name = "EnterRegionRequest";
    std::string PlayerId, Region;

};
struct JoinRegion
{
    static constexpr auto packet_name = "JoinRegion";
    std::string Region;
    int Index = Names::first_index;

};
struct RegionFull
{
    static constexpr auto packet_name = "RegionFull";
    std::string Reason, Region;
    int Index = Names::first_index;

};
struct RegionUnavailable
{
    static constexpr auto packet_name = "RegionUnavailable";
    std::string Region, Reason;

};
struct MoveRequest
{
    static constexpr auto packet_name = "MoveRequest";
    double Delta = 0;
    std::int64_t Sequence = 0;

};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string Reason;
    std::int32_t RetryAfterMs = 0;
    bool operator== (const ActionRejected &) const = default;

};
struct PlayerPosition
{
    std::string PlayerId;
    double X = 0;
    std::int64_t Sequence = 0;
    bool operator== (const PlayerPosition &) const = default;

};
struct RegionView
{
    static constexpr auto packet_name = "RegionView";
    std::string RegionId, NodeId;
    std::vector<PlayerPosition> Players;

};
struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};

struct DungeonInfoRequest
{
    static constexpr auto packet_name = "DungeonInfoRequest";
};

struct OpenDungeonRequest
{
    static constexpr auto packet_name = "OpenDungeonRequest";
    std::string DungeonId;

};
struct DungeonOpened
{
    static constexpr auto packet_name = "DungeonOpened";
    std::string DungeonId, NodeId;

};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::SessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::ActorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::ObjectGeneration)
    };
};

template <> struct dto_traits<::MyGame::EnterRegionRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::EnterRegionRequest::PlayerId),
      zlink::json::field ("region", &::MyGame::EnterRegionRequest::Region)
    };
};

template <> struct dto_traits<::MyGame::JoinRegion>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("region", &::MyGame::JoinRegion::Region),
      zlink::json::field ("index", &::MyGame::JoinRegion::Index)
    };
};

template <> struct dto_traits<::MyGame::RegionFull>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::RegionFull::Reason),
      zlink::json::field ("region", &::MyGame::RegionFull::Region),
      zlink::json::field ("index", &::MyGame::RegionFull::Index)
    };
};

template <> struct dto_traits<::MyGame::RegionUnavailable>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("region", &::MyGame::RegionUnavailable::Region),
      zlink::json::field ("reason", &::MyGame::RegionUnavailable::Reason)
    };
};

template <> struct dto_traits<::MyGame::MoveRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("delta", &::MyGame::MoveRequest::Delta),
      zlink::json::field ("sequence", &::MyGame::MoveRequest::Sequence)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::Reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::RetryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::PlayerPosition>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::PlayerPosition::PlayerId),
      zlink::json::field ("x", &::MyGame::PlayerPosition::X),
      zlink::json::field ("sequence", &::MyGame::PlayerPosition::Sequence)
    };
};

template <> struct dto_traits<::MyGame::RegionView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("regionId", &::MyGame::RegionView::RegionId),
      zlink::json::field ("nodeId", &::MyGame::RegionView::NodeId),
      zlink::json::field ("players", &::MyGame::RegionView::Players)
    };
};

template <> struct dto_traits<::MyGame::ViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::DungeonInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::OpenDungeonRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::OpenDungeonRequest::DungeonId)
    };
};

template <> struct dto_traits<::MyGame::DungeonOpened>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("dungeonId", &::MyGame::DungeonOpened::DungeonId),
      zlink::json::field ("nodeId", &::MyGame::DungeonOpened::NodeId)
    };
};
} // namespace zlink::json
