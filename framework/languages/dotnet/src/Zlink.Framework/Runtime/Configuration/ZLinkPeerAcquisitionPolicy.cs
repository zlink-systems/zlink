namespace Zlink.Framework.Runtime.Configuration;

internal enum ZLinkPeerAcquisitionMode
{
    None,
    AutoConnect,
    Manual
}

internal static class ZLinkPeerAcquisitionPolicy
{
    public static ZLinkPeerAcquisitionMode Resolve(
        bool autoConnectConfigured,
        IReadOnlyCollection<string> manualConnections)
    {
        if (manualConnections.Count > 0) return ZLinkPeerAcquisitionMode.Manual;

        return autoConnectConfigured
            ? ZLinkPeerAcquisitionMode.AutoConnect
            : ZLinkPeerAcquisitionMode.None;
    }

    public static void RequirePeerSource(
        string capabilityName,
        bool autoConnectConfigured,
        IReadOnlyCollection<string> manualConnections)
    {
        if (Resolve(autoConnectConfigured, manualConnections) == ZLinkPeerAcquisitionMode.None)
            throw new ZLinkConfigurationException(
                $"{capabilityName} requires location auto connect or manual connections.");
    }
}