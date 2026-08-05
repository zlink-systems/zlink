using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

/// <summary>
/// Connection, key namespace, and operation timeout settings for the Redis
/// Location Store.
/// </summary>
public sealed class ZLinkRedisLocationOptions
{
    /// <summary>StackExchange.Redis connection string, for example
    /// "127.0.0.1:6379". Ignored when <see cref="ConfigurationOptions"/> is
    /// set. One of the two must be provided.</summary>
    public string? ConnectionString { get; set; }

    /// <summary>Full connection configuration for callers that need more
    /// than a connection string. Takes precedence over
    /// <see cref="ConnectionString"/>.</summary>
    public ConfigurationOptions? ConfigurationOptions { get; set; }

    /// <summary>Required key namespace, for example "zlink:e2e".</summary>
    public string KeyPrefix { get; set; } = string.Empty;

    /// <summary>Maximum time allowed for one Store operation, including
    /// connection acquisition and the Redis command.</summary>
    public TimeSpan OperationTimeout { get; set; } = TimeSpan.FromSeconds(5);

    internal ConfigurationOptions BuildConfiguration()
    {
        var configuration = ConfigurationOptions is { } explicitOptions
            ? explicitOptions.Clone()
            : ConfigurationOptions.Parse(ConnectionString!);

        // The store maps command failures to StoreFailure / read
        // exceptions itself; the multiplexer must keep reconnecting in the
        // background instead of failing construction permanently.
        configuration.AbortOnConnectFail = false;
        return configuration;
    }

    internal void Validate()
    {
        if (string.IsNullOrEmpty(KeyPrefix))
        {
            throw new ArgumentException(
                "ZLinkRedisLocationOptions.KeyPrefix is required. Pick a namespace such as \"zlink:e2e\".",
                nameof(KeyPrefix));
        }

        if (ConfigurationOptions is null && string.IsNullOrEmpty(ConnectionString))
        {
            throw new ArgumentException(
                "ZLinkRedisLocationOptions requires ConnectionString or ConfigurationOptions.",
                nameof(ConnectionString));
        }

        if (OperationTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(OperationTimeout),
                "OperationTimeout must be greater than zero.");
    }
}
