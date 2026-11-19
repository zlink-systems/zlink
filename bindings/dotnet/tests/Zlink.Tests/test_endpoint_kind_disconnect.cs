using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     Core socket spec zlink_disconnect and zlink_unbind: each call ends only the
///     registrations of its own kind for the endpoint string.
/// </summary>
public sealed class test_endpoint_kind_disconnect
{
    private const int ObserveMs = 500;

    private static bool SawDisconnected(ISocketMonitor monitor, int observeMs)
    {
        long deadline = Environment.TickCount64 + observeMs;
        while (Environment.TickCount64 < deadline)
        {
            MonitorEvent? e = monitor.Recv(RecvFlags.DontWait);
            if (e == null)
            {
                Thread.Sleep(5);
                continue;
            }
            if (e.Event == MonitorEventType.Disconnected)
                return true;
        }
        return false;
    }

    private static void WaitReady(ISocketMonitor monitor)
    {
        long deadline = Environment.TickCount64 + 3000;
        while (Environment.TickCount64 < deadline)
        {
            MonitorEvent? e = monitor.Recv(RecvFlags.DontWait);
            if (e == null)
            {
                Thread.Sleep(5);
                continue;
            }
            if (e.Event == MonitorEventType.ConnectionReady)
                return;
        }
        Assert.Fail("client connection did not become ready");
    }

    [Theory]
    [InlineData("tcp://127.0.0.1:*")]
    [InlineData("inproc://endpoint-kind-keeps-bind")]
    public void disconnect_keeps_the_listener_and_inbound_connections(string address)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateDealerSocket();
        server.Options.Linger = client.Options.Linger = TimeSpan.Zero;
        server.Bind(address);
        string endpoint = server.Options.LastEndpoint;
        using ISocketMonitor monitor = client.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);
        client.Connect(endpoint);
        WaitReady(monitor);

        server.Connect(endpoint);
        server.Disconnect(endpoint);

        Assert.False(SawDisconnected(monitor, ObserveMs));
        server.Unbind(endpoint);
    }

    [Theory]
    [InlineData("tcp://127.0.0.1:*")]
    [InlineData("inproc://endpoint-kind-bind-only")]
    public void disconnect_of_a_bind_only_endpoint_is_not_found_and_keeps_the_listener(string address)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateDealerSocket();
        server.Options.Linger = client.Options.Linger = TimeSpan.Zero;
        server.Bind(address);
        string endpoint = server.Options.LastEndpoint;
        using ISocketMonitor monitor = client.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);
        client.Connect(endpoint);
        WaitReady(monitor);

        var failure = Assert.Throws<ZlinkConnectException>(() => server.Disconnect(endpoint));
        Assert.Equal(ZlinkConnectException.ErrorCode.NotFound, failure.Result);

        Assert.False(SawDisconnected(monitor, ObserveMs));
        server.Unbind(endpoint);
    }
}
