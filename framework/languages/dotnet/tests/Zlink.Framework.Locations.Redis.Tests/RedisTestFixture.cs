using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis.Tests;

/// <summary>
/// Probes a real Redis once per test run. When no Redis is reachable the
/// integration tests skip (never fail) with a message explaining how to
/// start one. Every store created through the fixture gets its own key
/// prefix under one run-unique namespace, and the whole namespace is
/// deleted when the run ends (draft 22절: dedicated prefix + cleanup).
/// </summary>
public sealed class RedisTestFixture : IAsyncLifetime
{
    private ConnectionMultiplexer? _cleanupConnection;
    private int _storeIndex;

    public string RunKeyPrefix { get; } = $"zlink:test:{Guid.NewGuid():N}";

    public string ConnectionString { get; private set; } = string.Empty;

    public string SkipReason { get; private set; } = "Redis probe has not run.";

    public bool RedisAvailable { get; private set; }

    public async Task InitializeAsync()
    {
        string?[] candidates =
        [
            Environment.GetEnvironmentVariable("ZLINK_REDIS_TEST_ENDPOINT"),
            "127.0.0.1:16379",
            "127.0.0.1:6379"
        ];
        foreach (var endpoint in candidates)
        {
            if (string.IsNullOrEmpty(endpoint))
            {
                continue;
            }

            try
            {
                var configuration = ConfigurationOptions.Parse(endpoint);
                configuration.ConnectTimeout = 2000;
                configuration.AbortOnConnectFail = true;
                var connection = await ConnectionMultiplexer.ConnectAsync(configuration);
                await connection.GetDatabase().PingAsync();
                _cleanupConnection = connection;
                ConnectionString = endpoint;
                RedisAvailable = true;
                return;
            }
            catch (Exception)
            {
                // Try the next candidate; unreachable Redis means skip.
            }
        }

        SkipReason =
            "Redis is not reachable (tried ZLINK_REDIS_TEST_ENDPOINT, 127.0.0.1:16379, 127.0.0.1:6379). "
            + "Start one with: docker run -d --rm --name zlink-redis-test "
            + "-p 127.0.0.1:16379:6379 redis:7-alpine";
    }

    public async Task DisposeAsync()
    {
        if (_cleanupConnection is null)
        {
            return;
        }

        foreach (var endpoint in _cleanupConnection.GetEndPoints())
        {
            var server = _cleanupConnection.GetServer(endpoint);
            var keys = new List<RedisKey>();
            await foreach (var key in server.KeysAsync(pattern: RunKeyPrefix + "*"))
            {
                keys.Add(key);
            }

            if (keys.Count > 0)
            {
                await _cleanupConnection.GetDatabase().KeyDeleteAsync([.. keys]);
            }
        }

        await _cleanupConnection.DisposeAsync();
    }

    /// <summary>Creates a store over an isolated key prefix so tests cannot
    /// observe each other's rows.</summary>
    public ZLinkRedisLocationStore CreateStore() =>
        CreateStore(out _);

    public ZLinkRedisLocationStore CreateStore(out string keyPrefix)
    {
        keyPrefix = $"{RunKeyPrefix}:{Interlocked.Increment(ref _storeIndex)}";
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = ConnectionString,
            KeyPrefix = keyPrefix
        });
    }

    public ZLinkRedisRelocationStore CreateRelocationStore()
    {
        var keyPrefix =
            $"{RunKeyPrefix}:{Interlocked.Increment(ref _storeIndex)}";
        return new ZLinkRedisRelocationStore(
            new ZLinkRedisRelocationOptions
            {
                ConnectionString = ConnectionString,
                KeyPrefix = keyPrefix
            });
    }

}

[CollectionDefinition(Name)]
public sealed class RedisTestCollection : ICollectionFixture<RedisTestFixture>
{
    public const string Name = "redis-location-store";
}
