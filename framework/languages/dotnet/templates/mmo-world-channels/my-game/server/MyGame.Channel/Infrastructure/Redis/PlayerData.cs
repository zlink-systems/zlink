using MyGame.Shared;
using StackExchange.Redis;

namespace MyGame.Channel.Infrastructure.Redis;

public sealed class PlayerData(NodeSettings settings) : IAsyncDisposable
{
    private readonly Task<ConnectionMultiplexer> connection = ConnectionMultiplexer.ConnectAsync(
        settings.RedisEndpoint
    );

    private string Key(string playerId) =>
        settings.KeyPrefix + GameNames.PlayerKeyPrefix + playerId;

    public async Task<long> LoadAsync(string playerId, CancellationToken ct)
    {
        var redis = await connection.WaitAsync(ct);
        var value = await redis.GetDatabase().StringGetAsync(Key(playerId)).WaitAsync(ct);
        return value.IsNull ? 0 : (long)value;
    }

    public async Task SaveAsync(string playerId, long gold, CancellationToken ct)
    {
        var redis = await connection.WaitAsync(ct);
        ct.ThrowIfCancellationRequested();
        // Complete an admitted write before updating the Actor; cancellation cannot detach it.
        if (!await redis.GetDatabase().StringSetAsync(Key(playerId), gold))
            throw new IOException("Player data write was rejected.");
    }

    public async ValueTask DisposeAsync()
    {
        var redis = await connection;
        await redis.CloseAsync();
        redis.Dispose();
    }
}
