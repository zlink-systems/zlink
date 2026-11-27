#include "redis_player_records.hpp"
#include "server/shared/hosting.hpp"
#include <charconv>

namespace MyGame
{
struct LoadPlayerHandler
{
    RedisPlayerRecords &records;
    using request_type = LoadPlayer;
    using reply_type = PlayerRecord;
    fx::task_t<PlayerRecord> handle (const LoadPlayer &request)
    {
        return records.load_player (request);
    }
};

struct CommitRoundHandler
{
    RedisPlayerRecords &records;
    using request_type = CommitRound;
    using reply_type = RoundCommitted;
    fx::task_t<RoundCommitted> handle (const CommitRound &request)
    {
        return records.commit_round (request);
    }
};
}

int main (int argc, char **argv)
{
    try {
        if (argc != 2)
            throw std::invalid_argument ("Usage: my-game-api <config.json>");
        const auto config = MyGame::NodeConfig::load (argv[1]);
        const auto &endpoint = config.ApiEndpoint.value ();
        const auto colon = endpoint.find_last_of (':');
        if (!endpoint.starts_with ("tcp://") || colon <= 6 || colon == std::string::npos)
            throw std::invalid_argument ("ApiEndpoint must be a TCP endpoint");
        const auto host = endpoint.substr (6, colon - 6);
        std::uint16_t port;
        const auto end = endpoint.data () + endpoint.size ();
        const auto [next, error] = std::from_chars (endpoint.data () + colon + 1, end, port);
        if (error != std::errc{} || next != end)
            throw std::invalid_argument ("ApiEndpoint must have a valid port");
        auto app = MyGame::fx::app_t::create ();
        auto &options = MyGame::configure_host (app, config);
        options.services ().add_singleton (std::make_unique<MyGame::RedisPlayerRecords> (config));
        options.services ().add_scoped<MyGame::LoadPlayerHandler, MyGame::RedisPlayerRecords> ();
        options.services ().add_scoped<MyGame::CommitRoundHandler, MyGame::RedisPlayerRecords> ();
        options.add_client_server_channel (MyGame::player_records_channel)
          .server ()
          .set_bind_host (host)
          .set_advertise_host (host)
          .listen (port)
          .add_request_handler<MyGame::LoadPlayerHandler, MyGame::LoadPlayer,
                               MyGame::PlayerRecord> ()
          .add_request_handler<MyGame::CommitRoundHandler, MyGame::CommitRound,
                               MyGame::RoundCommitted> ();
        return MyGame::run_host (app, config, false, argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}
