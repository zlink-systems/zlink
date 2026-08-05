using System;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_monitor_contract
{
    [Fact]
    public void socket_monitor_receive_reports_connection_ready_event()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "monitor-recv");
        server.Bind(endpoint);

        using ISocketMonitor monitor = server.MonitorOpen();
        client.Connect(endpoint);

        SocketMonitorEvent evt = monitor.Recv()
            ?? throw new TimeoutException("Timed out waiting for monitor event.");
        Assert.Equal(MonitorEventType.Accepted, evt.Event);
    }

    [Fact]
    public void socket_monitor_attach_handler_snapshot_and_close_contract()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        const ulong sendHwmBytes = (ulong)int.MaxValue + 4096UL;
        const ulong receiveHwmBytes = (ulong)int.MaxValue + 8192UL;
        server.Options.SendHighWaterMark = sendHwmBytes;
        server.Options.ReceiveHighWaterMark = receiveHwmBytes;
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "monitor-handler");
        server.Bind(endpoint);

        using var events = new CallbackEventQueue<SocketMonitorEvent>();
        using ISocketMonitor monitor = server.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);
        monitor.OnEvent(events.Enqueue);

        client.Connect(endpoint);

        Assert.True(events.TryDequeue(20000, out SocketMonitorEvent evt));
        Assert.Equal(MonitorEventType.ConnectionReady, evt.Event);

        MonitorStatus snapshot = monitor.Status();
        Assert.Equal(2U, snapshot.AbiVersion);
        Assert.True(snapshot.StructSize >= 232U);
        Assert.Equal<MonitorSourceKind>(MonitorSourceKind.Socket, snapshot.SourceKind);
        Assert.True(snapshot.SndPendingMsgs >= 0);
        Assert.True(Enum.IsDefined(snapshot.AutoHwmProfile));
        Assert.True(snapshot.AutoHwmPolicyClass >= 0);
        Assert.True(snapshot.AutoHwmUnitBudgetBytes >= 0);
        Assert.True(snapshot.AutoHwmSizeCap >= 0);
        Assert.True(snapshot.AutoHwmSocketMessageSlots >= 0);
        Assert.True(snapshot.AutoHwmConnectionBucketCount >= 0);
        Assert.True(snapshot.AutoHwmConnectionBucketHwm4K >= 0);
        Assert.Equal(sendHwmBytes,
            snapshot.AutoHwmAppliedSendHighWaterMarkBytes);
        Assert.Equal(receiveHwmBytes,
            snapshot.AutoHwmAppliedReceiveHighWaterMarkBytes);
        Assert.True(snapshot.MinimumCoreMessageChargeBytes > 0);

        monitor.Close();
        Assert.Throws<ObjectDisposedException>(() => monitor.Status());
    }

    [Fact]
    public void socket_monitor_try_receive_returns_null_when_queue_empty()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "monitor-try-recv-empty");
        server.Bind(endpoint);

        using ISocketMonitor monitor = server.MonitorOpen();

        Assert.Null(monitor.Recv(RecvFlags.DontWait));
    }

    [Fact]
    public void socket_monitor_poll_reports_pending_event()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "monitor-poll");
        server.Bind(endpoint);

        using ISocketMonitor monitor = server.MonitorOpen(SocketEvent.ConnectionReady);
        client.Connect(endpoint);

        Assert.Equal(1, ZlinkPoll.Poll(new[] { monitor }, 3000));
        SocketMonitorEvent evt = monitor.Recv(RecvFlags.DontWait)
            ?? throw new TimeoutException("Poll reported a monitor event but Recv was empty.");
        Assert.Equal(MonitorEventType.ConnectionReady, evt.Event);
    }

    [Fact]
    public void socket_monitor_ignore_handler_switches_to_callback_only_model()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "monitor-ignore");
        server.Bind(endpoint);

        using ISocketMonitor monitor = server.MonitorOpen(SocketEvent.ConnectionReady);
        monitor.OnEvent(_ => { });

        client.Connect(endpoint);

        Assert.True(CoreTestSupport.WaitUntil(() =>
        {
            try
            {
                _ = monitor.Status();
                return true;
            }
            catch (ZlinkConfigException)
            {
                return false;
            }
        }, 3000));

        ZlinkRecvException error = Assert.Throws<ZlinkRecvException>(() => monitor.Recv());
        Assert.Equal(ZlinkRecvException.ErrorCode.Busy, error.Result);
    }
}
