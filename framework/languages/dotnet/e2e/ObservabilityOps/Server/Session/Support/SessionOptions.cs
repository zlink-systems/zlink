namespace ObservabilityOps.Server.Session.Support;

using Zlink.Framework.E2E.Configuration;

internal sealed record SessionOptions(
    string Rid, string HttpUrl, string RedisEndpoint, string RedisKeyPrefix,
    string RouterEndpoint, string PubEndpoint, string StreamEndpoint,
    string LogDir)
{
    public static SessionOptions Parse(string[] args)
        => E2eConfiguration.Load<SessionOptions>(args);
}
