namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

internal sealed class ZLinkBackendSocketMonitorWrapper(ISocketMonitor nativeMonitor) : IZLinkBackendSocketMonitor
{
    private readonly ISocketMonitor[] _monitors = [nativeMonitor];

    public bool Wait(TimeSpan timeout)
    {
        var milliseconds = timeout <= TimeSpan.Zero
            ? 0
            : timeout.TotalMilliseconds >= int.MaxValue
                ? int.MaxValue
                : (int)Math.Ceiling(timeout.TotalMilliseconds);
        return ZlinkPoll.Poll(_monitors, milliseconds) > 0;
    }

    public void OnEvent(Action<ZLinkBackendSocketMonitorEvent> handler)
    {
        nativeMonitor.OnEvent(monitorEvent => handler(monitorEvent.ToFramework()));
    }

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
