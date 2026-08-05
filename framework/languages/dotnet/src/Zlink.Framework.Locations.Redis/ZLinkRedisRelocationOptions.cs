using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

/// <summary>
/// Connection and key namespace settings for the official Redis relocation
/// payload store.
/// </summary>
public sealed class ZLinkRedisRelocationOptions
{
    public string? ConnectionString { get; set; }

    public ConfigurationOptions? ConfigurationOptions { get; set; }

    public string KeyPrefix { get; set; } = string.Empty;

    public TimeSpan OperationTimeout { get; set; } = TimeSpan.FromSeconds(5);

    internal ConfigurationOptions BuildConfiguration()
    {
        var configuration = ConfigurationOptions is { } explicitOptions
            ? explicitOptions.Clone()
            : ConfigurationOptions.Parse(ConnectionString!);
        configuration.AbortOnConnectFail = false;
        return configuration;
    }

    internal void Validate()
    {
        if (string.IsNullOrEmpty(KeyPrefix))
        {
            throw new ArgumentException(
                "ZLinkRedisRelocationOptions.KeyPrefix is required.",
                nameof(KeyPrefix));
        }

        if (ConfigurationOptions is null && string.IsNullOrEmpty(ConnectionString))
        {
            throw new ArgumentException(
                "ZLinkRedisRelocationOptions requires ConnectionString or ConfigurationOptions.",
                nameof(ConnectionString));
        }

        if (OperationTimeout <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(OperationTimeout),
                "OperationTimeout must be greater than zero.");
    }
}
