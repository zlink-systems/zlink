// SPDX-License-Identifier: MPL-2.0

using System;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

// core-byte-hwm-flow-control-plan.ko.md §5.1/§7.3/§8.1.1: dotnet binding
// parity for the paired DEALER/ROUTER receive-flow-state surface.
public sealed class test_flow_state
{
    [Fact]
    public void receive_flow_state_enum_matches_c_abi_values()
    {
        Assert.Equal(0, (int)ReceiveFlowState.Running);
        Assert.Equal(1, (int)ReceiveFlowState.Paused);
    }

    [Fact]
    public void dealer_and_router_accept_receive_flow_state_and_are_idempotent()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "flow-state-dr");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(50);

        // First application and idempotent repeats both succeed on both
        // sides of the pair.
        dealer.SetReceiveFlowState(ReceiveFlowState.Paused);
        dealer.SetReceiveFlowState(ReceiveFlowState.Paused);
        dealer.SetReceiveFlowState(ReceiveFlowState.Running);
        dealer.SetReceiveFlowState(ReceiveFlowState.Running);

        router.SetReceiveFlowState(ReceiveFlowState.Paused);
        router.SetReceiveFlowState(ReceiveFlowState.Paused);
        router.SetReceiveFlowState(ReceiveFlowState.Running);
        router.SetReceiveFlowState(ReceiveFlowState.Running);
    }

    // core-byte-hwm-flow-control-plan.ko.md §6: PAUSED/RESUMED fire on the
    // *sender* whose pipe was actually blocked/unblocked, carrying routing
    // id, pair id/generation, a flow epoch in Value, and the
    // SendFlowWritable flag on RESUMED. Mirrors
    // core/tests/integration/test_flow_state_c_api.cpp's
    // test_pause_and_resume_each_emit_exactly_one_event.
    [Fact]
    public void pause_and_resume_report_flow_events_with_full_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        using var dealer = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "tcp", "flow-state-events");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        // One round trip so the ROUTER learns the route and both
        // transport-pair lanes are ready.
        CoreTestSupport.SendAsyncWithTimeout(dealer, "hello"u8, 2000);
        var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(
            () => router.Recv(received, RecvFlags.DontWait), 2000));
        received.Dispose();

        using var events = new CallbackEventQueue<SocketMonitorEvent>();
        using ISocketMonitor monitor = dealer.MonitorOpen(
            SocketEvent.SendFlowPaused | SocketEvent.SendFlowResumed);
        monitor.OnEvent(events.Enqueue);

        // The ROUTER asking to pause blocks the DEALER's send pipe, so the
        // event is observed on the DEALER's monitor, not the ROUTER's.
        router.SetReceiveFlowState(ReceiveFlowState.Paused);

        Assert.True(events.TryDequeue(2000, out SocketMonitorEvent paused));
        Assert.Equal(MonitorEventType.SendFlowPaused, paused.Event);
        Assert.NotEqual(0UL, paused.Value);
        Assert.NotEqual(0UL, paused.TransportPairId);
        Assert.NotNull(paused.RoutingId);
        Assert.Equal(MonitorEventFlags.None,
            paused.Flags & MonitorEventFlags.SendFlowWritable);

        router.SetReceiveFlowState(ReceiveFlowState.Running);

        Assert.True(events.TryDequeue(2000, out SocketMonitorEvent resumed));
        Assert.Equal(MonitorEventType.SendFlowResumed, resumed.Event);
        Assert.True(resumed.Value > paused.Value);
        Assert.Equal(paused.TransportPairId, resumed.TransportPairId);
        Assert.Equal(paused.TransportPairGeneration,
            resumed.TransportPairGeneration);
        Assert.Equal(MonitorEventFlags.SendFlowWritable,
            resumed.Flags & MonitorEventFlags.SendFlowWritable);
    }

    [Fact]
    public void unsupported_socket_types_report_not_supported()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var pair = ctx.CreatePairSocket();
        using var pub = ctx.CreatePubSocket();
        using var sub = ctx.CreateSubSocket();
        using var xpub = ctx.CreateXPubSocket();
        using var xsub = ctx.CreateXSubSocket();
        using var stream = ctx.CreateStreamSocket();

        foreach (ISocket socket in new ISocket[]
                 {
                     pair, pub, sub, xpub, xsub, stream
                 })
        {
            var ex = Assert.Throws<ZlinkConfigException>(
                () => socket.SetReceiveFlowState(ReceiveFlowState.Paused));
            Assert.Equal(ZlinkConfigException.ErrorCode.NotSupported, ex.Result);
        }
    }

    [Fact]
    public void unsupported_socket_send_recv_is_unchanged_after_flow_state_calls()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "flow-state-pair");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(50);

        Assert.Throws<ZlinkConfigException>(
            () => client.SetReceiveFlowState(ReceiveFlowState.Paused));

        using Message payload = Message.From("still-works");
        client.Send().Message(payload).Submit();

        var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(
            () => server.Recv(received, RecvFlags.DontWait), 2000));
        Assert.Equal("still-works", received.Parts[0].GetString());
        received.Dispose();
    }

    [Fact]
    public void out_of_range_state_is_invalid_argument()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();

        foreach (int rawValue in new[] { 2, -1, 999 })
        {
            var ex = Assert.Throws<ZlinkConfigException>(() =>
                dealer.SetReceiveFlowState((ReceiveFlowState)rawValue));
            Assert.Equal(ZlinkConfigException.ErrorCode.InvalidArgument, ex.Result);
        }
    }

    [Fact]
    public void closed_socket_reports_object_disposed()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        var dealer = ctx.CreateDealerSocket();
        dealer.Close();

        // Matches this binding's existing closed-handle policy for other
        // config-like calls on SocketBase (e.g. monitor Status() after
        // Close()): the managed handle gate throws before any native call.
        Assert.Throws<ObjectDisposedException>(
            () => dealer.SetReceiveFlowState(ReceiveFlowState.Running));
    }

    [Fact]
    public async Task set_receive_flow_state_racing_close_observes_only_bounded_outcomes()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        for (var iteration = 0; iteration < 20; iteration++)
        {
            using var ctx = Zlink.CreateContext();
            var dealer = ctx.CreateDealerSocket();

            var barrier = new Barrier(2);
            Task setTask = Task.Run(() =>
            {
                barrier.SignalAndWait();
                try
                {
                    dealer.SetReceiveFlowState(ReceiveFlowState.Paused);
                }
                catch (ObjectDisposedException)
                {
                    // Close won the managed handle race first.
                }
                catch (ZlinkConfigException ex)
                {
                    Assert.True(
                        ex.Result is ZlinkConfigException.ErrorCode.InvalidState
                            or ZlinkConfigException.ErrorCode.InvalidHandle,
                        $"unexpected result {ex.Result}");
                }
            });
            Task closeTask = Task.Run(() =>
            {
                barrier.SignalAndWait();
                dealer.Close();
            });

            await Task.WhenAll(setTask, closeTask)
                .WaitAsync(TimeSpan.FromSeconds(5));
            barrier.Dispose();
        }
    }

    [Fact]
    public void public_surface_has_no_flow_frame_or_pause_bypass_api()
    {
        Type[] socketTypes =
        {
            typeof(ISocket), typeof(IPairSocket), typeof(IDealerSocket),
            typeof(IRouterSocket), typeof(IPubSocket), typeof(ISubSocket),
            typeof(IXPubSocket), typeof(IXSubSocket), typeof(IStreamSocket)
        };

        foreach (Type socketType in socketTypes)
        {
            MethodInfo[] methods = socketType.IsInterface
                ? socketType.GetInterfaces().Append(socketType)
                    .SelectMany(current => current.GetMethods()).ToArray()
                : socketType.GetMethods(
                    BindingFlags.Instance | BindingFlags.Public);

            foreach (MethodInfo method in methods)
            {
                bool nameMentionsFlow = method.Name.Contains(
                    "Flow", StringComparison.OrdinalIgnoreCase);
                if (!nameMentionsFlow)
                    continue;

                Assert.Equal(nameof(ISocket.SetReceiveFlowState), method.Name);
            }
        }

        Assert.DoesNotContain(typeof(Zlink).Assembly.GetExportedTypes(),
            type => type.Name.Contains("FlowFrame", StringComparison.Ordinal)
                || type.Name.Contains("PauseBypass", StringComparison.Ordinal));
    }
}
