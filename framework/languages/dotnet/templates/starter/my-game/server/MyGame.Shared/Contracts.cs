namespace MyGame.Shared;

public static class GameNames
{
    public const string WorldMesh = "world";
    public const string SessionStream = "session";
}

public sealed record NodeReady(string Id, string MeshEndpoint, string? StreamEndpoint);

public static class NodeProtocol
{
    public const string ReadyPrefix = "READY ";
    public const string Drain = "drain";
    public const string Shutdown = "shutdown";
    public const string RelocationRequested = "RELOCATION_REQUESTED";
    public const string Relocated = "RELOCATED";
    public const string SafeToShutdown = "SAFE_TO_SHUTDOWN";

    public static bool IsMarker(string line) =>
        line.StartsWith(ReadyPrefix, StringComparison.Ordinal)
        || line is RelocationRequested or Relocated or SafeToShutdown;
}
