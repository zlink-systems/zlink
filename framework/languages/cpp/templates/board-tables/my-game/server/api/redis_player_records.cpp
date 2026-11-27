#include "redis_player_records.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/detached.hpp>
#include <boost/redis/src.hpp>
#include <charconv>
#include <set>
#include <thread>

namespace MyGame
{
namespace fx = zlink::framework;
namespace redis = boost::redis;
namespace asio = boost::asio;

namespace
{
constexpr auto load_player_script = R"(
local chips = redis.call('GET', KEYS[1])
if not chips then
    redis.call('SET', KEYS[1], ARGV[1])
    return ARGV[1]
end
return chips
)";

// Redis runs the marker check and the write atomically across API processes.
constexpr auto commit_round_script = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
    local writes = {KEYS[1], '1'}
    for i = 2, #KEYS do
        writes[#writes + 1] = KEYS[i]
        writes[#writes + 1] = ARGV[i - 1]
    end
    redis.call('MSET', unpack(writes))
end
return 1
)";
}

struct RedisPlayerRecords::Connection
{
    asio::io_context io;
    redis::connection client{io.get_executor ()};
    std::jthread thread;

    explicit Connection (const std::string &endpoint)
    {
        const auto colon = endpoint.find_last_of (':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == endpoint.size ())
            throw std::invalid_argument ("RedisEndpoint must have a host and port");
        redis::config config;
        config.addr.host = endpoint.substr (0, colon);
        config.addr.port = endpoint.substr (colon + 1);
        client.async_run (config, {}, asio::detached);
        thread = std::jthread ([this] { io.run (); });
    }

    ~Connection ()
    {
        asio::post (io, [this] { client.cancel (); });
        thread.join ();
    }

    template <class T> fx::task_t<T> execute (redis::request request)
    {
        redis::response<T> response;
        fx::task_completion_source_t<void> completed;
        asio::post (io, [&] {
            client.async_exec (
              request, response, [&] (const boost::system::error_code &error, auto) {
                  if (error)
                      completed.complete (fx::result_t<void>::failure (
                        fx::framework_error_kind_t::internal_failure, error.message ()));
                  else
                      completed.complete (fx::result_t<void>::success ());
              });
        });
        co_await completed.task ();
        co_return std::get<0> (response).value ();
    }
};

RedisPlayerRecords::RedisPlayerRecords (const NodeConfig &config) :
    redis_ (std::make_unique<Connection> (config.RedisEndpoint)),
    prefix_ (config.PlayerRecordsKeyPrefix.value_or (config.KeyPrefix))
{
}

RedisPlayerRecords::~RedisPlayerRecords () = default;

std::string RedisPlayerRecords::player_key (const std::string &player) const
{
    if (player.empty ())
        throw std::invalid_argument ("Player ID is required");
    return prefix_ + "players:" + player;
}

fx::task_t<PlayerRecord> RedisPlayerRecords::load_player (const LoadPlayer &request)
{
    redis::request command;
    command.push ("EVAL", load_player_script, 1, player_key (request.playerId),
                  Rules::initial_chips);
    const auto balance = co_await redis_->execute<std::string> (std::move (command));
    std::int64_t chips;
    const auto end = balance.data () + balance.size ();
    const auto [next, error] = std::from_chars (balance.data (), end, chips);
    if (error != std::errc{} || next != end || !Rules::is_contract_integer (chips))
        throw std::runtime_error ("Invalid player balance in Redis");
    co_return PlayerRecord{request.playerId, chips};
}

fx::task_t<RoundCommitted> RedisPlayerRecords::commit_round (const CommitRound &request)
{
    if (request.tableId.empty () || request.handNumber == 0
        || !Rules::is_contract_integer (request.handNumber)
        || request.results.size () < Rules::minimum_players
        || request.results.size () > Rules::seats)
        throw std::invalid_argument ("Round identity and all seat balances are required");

    std::vector<std::string> args{commit_round_script, std::to_string (request.results.size () + 1),
                                  prefix_ + "rounds:" + std::to_string (request.handNumber) + ":"
                                    + request.tableId};
    std::set<std::string_view> players;
    for (const auto &result : request.results) {
        if (!players.insert (result.playerId).second || !Rules::is_contract_integer (result.chips))
            throw std::invalid_argument (
              "Round results require unique player IDs and valid balances");
        args.push_back (player_key (result.playerId));
    }
    for (const auto &result : request.results)
        args.push_back (std::to_string (result.chips));
    redis::request command;
    command.push_range ("EVAL", args);
    (void) co_await redis_->execute<int> (std::move (command));
    co_return RoundCommitted{request.tableId, request.handNumber};
}
}
