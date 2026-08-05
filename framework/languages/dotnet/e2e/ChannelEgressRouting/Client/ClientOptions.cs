using Zlink.Framework.E2E.Configuration;

namespace ChannelEgressRouting.Client;

public sealed record ClientOptions(
    string Scenario,
    Dictionary<string, string> Urls,
    Dictionary<string, string> EvidenceFiles,
    string InvalidServerProject,
    string ConfigDir,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDir,
    string StreamEndpoint)
{
    public static ClientOptions Parse(string[] args) =>
        E2eConfiguration.Load<ClientOptions>(args);
}
