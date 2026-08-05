using System.Security.Cryptography;
using System.Text;
using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

internal static class ZLinkRedisConnectionPool
{
    private static readonly object Gate = new();
    private static readonly Dictionary<string, PoolEntry> Entries =
        new(StringComparer.Ordinal);

    internal static string CreateKey(ConfigurationOptions configuration)
    {
        ArgumentNullException.ThrowIfNull(configuration);
        var normalized = configuration.ToString(includePassword: true);
        return Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(normalized)))
            .ToLowerInvariant();
    }

    internal static Func<
        ConfigurationOptions,
        ValueTask<IZLinkRedisConnection>> CreateFactory(
            ConfigurationOptions configuration,
            bool shareConnection)
    {
        // Explicit ConfigurationOptions can carry callbacks that ToString()
        // cannot identify. Callers disable sharing for that configuration.
        if (!shareConnection)
            return ConnectAsync;

        var poolKey = CreateKey(configuration);
        return options => RentAsync(poolKey, options, ConnectAsync);
    }

    internal static async ValueTask<IZLinkRedisConnection> RentAsync(
        string key,
        ConfigurationOptions configuration,
        Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>> connect)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(key);
        ArgumentNullException.ThrowIfNull(configuration);
        ArgumentNullException.ThrowIfNull(connect);

        PoolEntry entry;
        lock (Gate)
        {
            if (!Entries.TryGetValue(key, out entry!))
            {
                entry = new PoolEntry(
                    connect(configuration.Clone()).AsTask());
                Entries.Add(key, entry);
            }

            entry.ReferenceCount++;
        }

        try
        {
            var connection = await entry.Connection.ConfigureAwait(false);
            return new ConnectionLease(key, entry, connection);
        }
        catch
        {
            await ReleaseAsync(key, entry, connectionWasPublished: false)
                .ConfigureAwait(false);
            throw;
        }
    }

    private static async ValueTask ReleaseAsync(
        string key,
        PoolEntry entry,
        bool connectionWasPublished)
    {
        var dispose = false;
        lock (Gate)
        {
            if (entry.ReferenceCount <= 0)
                return;
            entry.ReferenceCount--;
            if (entry.ReferenceCount == 0
                && Entries.TryGetValue(key, out var current)
                && ReferenceEquals(current, entry))
            {
                Entries.Remove(key);
                dispose = connectionWasPublished;
            }
        }

        if (!dispose)
            return;

        var connection = await entry.Connection.ConfigureAwait(false);
        await connection.DisposeAsync().ConfigureAwait(false);
    }

    private static async ValueTask<IZLinkRedisConnection> ConnectAsync(
        ConfigurationOptions options) =>
        new ZLinkStackExchangeRedisConnection(
            await ConnectionMultiplexer.ConnectAsync(options)
                .ConfigureAwait(false));

    private sealed class PoolEntry(Task<IZLinkRedisConnection> connection)
    {
        internal Task<IZLinkRedisConnection> Connection { get; } = connection;

        internal int ReferenceCount { get; set; }
    }

    private sealed class ConnectionLease(
        string key,
        PoolEntry entry,
        IZLinkRedisConnection connection) : IZLinkRedisConnection
    {
        private PoolEntry? _entry = entry;

        public IDatabase GetDatabase() => connection.GetDatabase();

        public async ValueTask DisposeAsync()
        {
            var owned = Interlocked.Exchange(ref _entry, null);
            if (owned is not null)
            {
                await ReleaseAsync(
                        key,
                        owned,
                        connectionWasPublished: true)
                    .ConfigureAwait(false);
            }
        }
    }
}
