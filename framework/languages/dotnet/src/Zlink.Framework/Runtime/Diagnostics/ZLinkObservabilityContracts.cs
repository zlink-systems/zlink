namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkMeters
{
    public const string Framework = "zlink.framework";
}

internal enum ZLinkFlowOrigin : byte
{
    Inbound = 1,
    Timer = 2,
    Application = 3,
    Lifecycle = 4
}
