using Zlink.Framework.E2E.Configuration;

namespace SubmitAdmission.Server;

internal sealed record ServerOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string? MeshEndpoint = null,
    string? PeerRid = null,
    string? PeerEndpoint = null,
    string? FanoutEndpoint = null,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null)
{
    public static ServerOptions Parse(string[] args) =>
        E2eConfiguration.Load<ServerOptions>(args);
}
