using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendSocketMonitorWrapper(ISocketMonitor nativeMonitor)
    : IZLinkBackendSocketMonitor
{
    public IZLinkBackendSocketPoller CreateReceivePoller() =>
        ZLinkBackendSocketPoller.Create(nativeMonitor);

    public bool TryRecv(out ZLinkBackendSocketMonitorEvent monitorEvent)
    {
        var nativeEvent = nativeMonitor.Recv(RecvFlags.DontWait);
        if (nativeEvent is null)
        {
            monitorEvent = default;
            return false;
        }

        monitorEvent = nativeEvent.ToFramework();
        return true;
    }

    public ValueTask DisposeAsync()
    {
        return nativeMonitor.DisposeAsync();
    }
}
