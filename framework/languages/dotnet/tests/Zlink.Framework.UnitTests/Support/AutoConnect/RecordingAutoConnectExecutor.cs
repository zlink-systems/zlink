using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

internal sealed class RecordingAutoConnectExecutor : IZLinkAutoConnectExecutor
{
    public List<ZLinkAutoConnectTarget> Connected { get; } = [];

    public List<ZLinkAutoConnectTarget> Disconnected { get; } = [];

    public bool ConnectSucceeds { get; set; } = true;

    public bool DisconnectSucceeds { get; set; } = true;

    public bool Connect(ZLinkAutoConnectTarget target)
    {
        Connected.Add(target);
        return ConnectSucceeds;
    }

    public bool Disconnect(ZLinkAutoConnectTarget target)
    {
        Disconnected.Add(target);
        return DisconnectSucceeds;
    }
}
