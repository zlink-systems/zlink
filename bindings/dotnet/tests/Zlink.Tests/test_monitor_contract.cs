using System;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_monitor_contract
{
    [Fact]
    public void socket_monitor_hwm_bytes_are_forwarded_exactly()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var socket = ctx.CreatePairSocket();
        const ulong monitorHwmBytes = 12_345UL;
        using ISocketMonitor monitor = socket.MonitorOpen(SocketEvent.All,
            monitorHwmBytes);

        Assert.Equal(monitorHwmBytes * 2UL,
            ctx.GetCoreHwmBudgetSnapshot().MonitorQueueAppliedHwmBytes);
    }

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
        Assert.Equal(4U, snapshot.AbiVersion);
        Assert.True(snapshot.StructSize > 0U);
        Assert.Equal<MonitorSourceKind>(MonitorSourceKind.Socket, snapshot.SourceKind);
        Assert.True(snapshot.SndPendingMsgs >= 0);
        Assert.True(Enum.IsDefined(snapshot.AutoHwmProfile));
        Assert.True(snapshot.AutoHwmPolicyClass >= 0);
        Assert.True(snapshot.SndPendingBytes >= 0);
        Assert.True(snapshot.RcvPendingBytes >= 0);
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

    // core-byte-hwm-flow-control-plan.ko.md §6 / core/include/zlink_enum.h:
    // the three flow-event bits and the widened ALL mask.
    [Fact]
    public void flow_event_enum_values_match_c_abi()
    {
        Assert.Equal(0x10000, (int)MonitorEventType.SendFlowPaused);
        Assert.Equal(0x20000, (int)MonitorEventType.SendFlowResumed);
        Assert.Equal(0x40000, (int)MonitorEventType.FlowStateStale);

        Assert.Equal(0x10000, (int)SocketEvent.SendFlowPaused);
        Assert.Equal(0x20000, (int)SocketEvent.SendFlowResumed);
        Assert.Equal(0x40000, (int)SocketEvent.FlowStateStale);
        Assert.Equal(0x7FFFF, (int)SocketEvent.All);
    }

    // core/include/zlink/eventing/api.h: ZLINK_MONITOR_EVENT_FLAG_*.
    [Fact]
    public void monitor_event_flag_values_match_c_abi()
    {
        Assert.Equal(1u << 0, (uint)MonitorEventFlags.ConnectionReadyEdge);
        Assert.Equal(1u << 1, (uint)MonitorEventFlags.SendFlowWritable);
        Assert.Equal(1u << 2, (uint)MonitorEventFlags.FlowStateStaleGeneration);
        Assert.Equal(1u << 3, (uint)MonitorEventFlags.FlowStateStaleEpoch);
    }

    [Fact]
    public void monitor_event_value_and_pair_fields_are_full_64_bit()
    {
        // Guards against the truncating `(uint)evt.Value` cast this section
        // fixed: the public record's fields must be wide enough to carry
        // the native uint64 payload without loss.
        Assert.Equal(typeof(ulong),
            typeof(MonitorEvent).GetProperty(nameof(MonitorEvent.Value))!
                .PropertyType);
        Assert.Equal(typeof(ulong),
            typeof(MonitorEvent).GetProperty(
                    nameof(MonitorEvent.TransportPairId))!
                .PropertyType);
        Assert.Equal(typeof(ulong),
            typeof(MonitorEvent).GetProperty(
                    nameof(MonitorEvent.TransportPairGeneration))!
                .PropertyType);
        Assert.Equal(typeof(MonitorEventFlags),
            typeof(MonitorEvent).GetProperty(nameof(MonitorEvent.Flags))!
                .PropertyType);

        const ulong beyondUInt32 = (ulong)uint.MaxValue + 1UL;
        var evt = new MonitorEvent(MonitorEventType.SendFlowPaused,
            beyondUInt32, null, string.Empty, string.Empty,
            beyondUInt32 + 1UL, beyondUInt32 + 2UL,
            MonitorEventFlags.SendFlowWritable);

        Assert.Equal(beyondUInt32, evt.Value);
        Assert.Equal(beyondUInt32 + 1UL, evt.TransportPairId);
        Assert.Equal(beyondUInt32 + 2UL, evt.TransportPairGeneration);
    }
}
