using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

internal sealed class RecordingAutoConnectExecutor : IZLinkAutoConnectExecutor
{
    public List<ZLinkAutoConnectTarget> Connected { get; } = [];

    public List<ZLinkAutoConnectTarget> Disconnected { get; } = [];

    public bool ConnectSucceeds { get; set; } = true;

    public bool DisconnectSucceeds { get; set; } = true;

    public ValueTask<bool> ConnectAsync(ZLinkAutoConnectTarget target)
    {
        Connected.Add(target);
        return ValueTask.FromResult(ConnectSucceeds);
    }

    public ValueTask<bool> DisconnectAsync(ZLinkAutoConnectTarget target)
    {
        Disconnected.Add(target);
        return ValueTask.FromResult(DisconnectSucceeds);
    }
}
