using System.Text.Json;
using StackExchange.Redis;

namespace GameQuest.Server.Configuration;

public sealed class RedisJsonStore : IAsyncDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    private readonly IDatabase _database;
    private readonly ConnectionMultiplexer _redis;

    public RedisJsonStore(string endpoint)
    {
        _redis = ConnectionMultiplexer.Connect(endpoint);
        _database = _redis.GetDatabase();
    }

    public async ValueTask DisposeAsync()
    {
        await _redis.CloseAsync().ConfigureAwait(false);
        _redis.Dispose();
    }

    public async ValueTask<T> ReadAsync<T>(
        string key,
        T fallback,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var json = await _database.StringGetAsync(key).ConfigureAwait(false);
        if (json.IsNullOrEmpty) return fallback;

        return JsonSerializer.Deserialize<T>(json!, JsonOptions) ?? fallback;
    }

    public async ValueTask<TResult> UpdateAsync<T, TResult>(
        string key,
        T fallback,
        Func<T, TResult> update,
        CancellationToken cancellationToken)
    {
        var lockKey = $"{key}:lock";
        var lockValue = Environment.MachineName + ":" + Environment.ProcessId + ":" + Guid.NewGuid().ToString("N");
        await AcquireLockAsync(lockKey, lockValue, cancellationToken).ConfigureAwait(false);
        try
        {
            var value = await ReadAsync(key, fallback, cancellationToken).ConfigureAwait(false);
            var result = update(value);
            await _database.StringSetAsync(key, JsonSerializer.Serialize(value, JsonOptions)).ConfigureAwait(false);
            return result;
        }
        finally
        {
            await _database.LockReleaseAsync(lockKey, lockValue).ConfigureAwait(false);
        }
    }

    public async ValueTask UpdateAsync<T>(
        string key,
        T fallback,
        Action<T> update,
        CancellationToken cancellationToken)
    {
        await UpdateAsync(
            key,
            fallback,
            value =>
            {
                update(value);
                return true;
            },
            cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask AcquireLockAsync(
        string lockKey,
        string lockValue,
        CancellationToken cancellationToken)
    {
        while (!await _database.LockTakeAsync(lockKey, lockValue, TimeSpan.FromSeconds(30)).ConfigureAwait(false))
            await Task.Delay(10, cancellationToken).ConfigureAwait(false);
    }
}
