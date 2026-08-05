using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class BackendMonitorWrapperTests
{
    [Fact]
    public void BackendMonitorWrapper_Uses_NonBlocking_Receive()
    {
        var monitor = new RecordingSocketMonitor(null);
        var wrapper = new ZLinkBackendSocketMonitorWrapper(monitor);

        var received = wrapper.TryRecv(out _);

        Assert.False(received);
        Assert.Equal(RecvFlags.DontWait, monitor.LastRecvFlags);
    }

    [Fact]
    public void BackendMonitorWrapper_Maps_Available_Event()
    {
        var nativeEvent = new MonitorEvent(
            MonitorEventType.Connected,
            7,
            RoutingId.From("peer-a"),
            "tcp://127.0.0.1:5001",
            "tcp://127.0.0.1:5002");
        var monitor = new RecordingSocketMonitor(nativeEvent);
        var wrapper = new ZLinkBackendSocketMonitorWrapper(monitor);

        var received = wrapper.TryRecv(out var monitorEvent);

        Assert.True(received);
        Assert.Equal(ZLinkSocketNativeEventType.Connected, monitorEvent.NativeEvent);
        Assert.Equal(nativeEvent.Value, monitorEvent.Value);
        Assert.Equal(nativeEvent.RoutingId, monitorEvent.RoutingId);
        Assert.Equal(nativeEvent.LocalAddr, monitorEvent.LocalAddr);
        Assert.Equal(nativeEvent.RemoteAddr, monitorEvent.RemoteAddr);
        Assert.Equal(RecvFlags.DontWait, monitor.LastRecvFlags);
    }

    private sealed class RecordingSocketMonitor(MonitorEvent? nextEvent) : ISocketMonitor
    {
        public RecvFlags? LastRecvFlags { get; private set; }

        public void OnEvent(Action<MonitorEvent> handler)
        {
        }

        public MonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
        {
            LastRecvFlags = flags;
            return nextEvent;
        }

        public MonitorStatus Status()
        {
            throw new NotSupportedException();
        }

        public void Close()
        {
        }

        public void Dispose()
        {
        }

        public ValueTask DisposeAsync()
        {
            return ValueTask.CompletedTask;
        }
    }
}
