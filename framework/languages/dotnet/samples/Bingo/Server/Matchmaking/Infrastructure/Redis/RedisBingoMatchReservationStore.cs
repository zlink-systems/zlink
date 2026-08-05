using System.Text.Json;
using Bingo.Server.Configuration;
using Bingo.Server.Matchmaking.Application;
using Bingo.Shared.Contracts;
using StackExchange.Redis;

namespace Bingo.Server.Matchmaking.Infrastructure.Redis;

internal sealed class RedisBingoMatchReservationStore(
    IConnectionMultiplexer redis,
    SampleRuntimeConfiguration<SampleMatchmakingNode> configuration)
    : IBingoMatchReservationStore
{
    private const string Script = """
        local key = KEYS[1]
        local actorId = ARGV[1]
        local roomId = ARGV[2]
        local settings = ARGV[3]
        local required = tonumber(ARGV[4])
        local nowMs = ARGV[5]
        local currentRoom = redis.call('HGET', key, 'RoomId')
        if not currentRoom then
          redis.call('HMSET', key, 'RoomId', roomId, 'Settings', settings,
            'ReservedActorIds', actorId, 'RequiredPlayers', required,
            'CreatedAtUnixMs', nowMs)
          return {roomId, settings}
        end
        local actors = redis.call('HGET', key, 'ReservedActorIds') or ''
        local currentSettings = redis.call('HGET', key, 'Settings')
        if string.find('|' .. actors .. '|', '|' .. actorId .. '|', 1, true) then
          return {currentRoom, currentSettings}
        end
        local count = 0
        for _ in string.gmatch(actors, '[^|]+') do count = count + 1 end
        if count >= required then
          redis.call('HMSET', key, 'RoomId', roomId, 'Settings', settings,
            'ReservedActorIds', actorId, 'RequiredPlayers', required,
            'CreatedAtUnixMs', nowMs)
          return {roomId, settings}
        end
        redis.call('HSET', key, 'ReservedActorIds', actors .. '|' .. actorId)
        return {currentRoom, currentSettings}
        """;

    public async ValueTask<ReserveBingoRoomRes> ReserveAsync(
        ReserveBingoRoomReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(request.Mode, BingoSampleModes.TwoPlayer, StringComparison.Ordinal))
            throw new InvalidOperationException($"Unsupported bingo mode. mode={request.Mode}");
        if (string.IsNullOrWhiteSpace(request.ActorId)
            || string.IsNullOrWhiteSpace(request.LevelBucket))
            throw new InvalidOperationException("Actor id and level bucket are required.");
        var roomId = $"bingo-room-{Guid.NewGuid():N}";
        var settings = new BingoRoomSettingsPayload
        {
            RoomName = $"Bingo Room {roomId[^6..]}",
            Mode = request.Mode,
            RequiredPlayers = 2,
            MaxDrawNumber = 15,
            Purpose = "Game"
        };
        var encoded = JsonSerializer.Serialize(settings);
        var result = (RedisResult[]?)await redis.GetDatabase().ScriptEvaluateAsync(
            Script,
            [$"{configuration.RedisKeyPrefix}match:{request.LevelBucket}:{request.Mode}"],
            [request.ActorId, roomId, encoded, 2,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()])
            ?? throw new InvalidOperationException("Redis reservation result is empty.");
        return new ReserveBingoRoomRes
        {
            RoomId = (string?)result[0]
                     ?? throw new InvalidOperationException("Redis reservation room id is empty."),
            Settings = JsonSerializer.Deserialize<BingoRoomSettingsPayload>(
                (string?)result[1]
                ?? throw new InvalidOperationException("Redis reservation settings are empty."))
        };
    }
}
