#pragma once
#include "server/shared/config.hpp"
#include "server/shared/player_records.hpp"
#include <zlink/framework.hpp>
#include <memory>

namespace MyGame
{
class RedisPlayerRecords
{
    struct Connection;
    std::unique_ptr<Connection> redis_;
    std::string prefix_;
    std::string player_key (const std::string &player) const;

  public:
    explicit RedisPlayerRecords (const NodeConfig &config);
    ~RedisPlayerRecords ();
    zlink::framework::task_t<PlayerRecord> load_player (const LoadPlayer &request);
    zlink::framework::task_t<RoundCommitted> commit_round (const CommitRound &request);
};
}
