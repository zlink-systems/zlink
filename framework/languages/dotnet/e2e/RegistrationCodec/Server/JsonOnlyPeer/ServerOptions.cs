namespace RegistrationCodec.Server.JsonOnlyPeer;

using Zlink.Framework.E2E.Configuration;

public sealed record ServerOptions(
    string Rid,
    string HttpUrl,
    string LogDir,
    string CodecMode,
    string? ChannelEndpoint = null,
    string? EvidenceFile = null,
    string? InvalidMode = null,
    string? JsonOnlyPeerProject = null)
{
    public static ServerOptions Parse(string[] args)
        => E2eConfiguration.Load<ServerOptions>(args);
}
