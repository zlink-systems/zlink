using System.Net;
using System.Net.Sockets;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_callback_contract
{
    [Fact]
    public void stream_packet_pull_transfers_ownership_and_output_is_reusable()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "packet-pull");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);
        using var client = new TcpClient();
        client.Connect(IPAddress.Loopback, port);

        using var packet = StreamPacket.Create();
        CoreTestSupport.SendStreamPacket(client.GetStream(), "first"u8);
        Assert.True(stream.RecvPacket(packet));
        Assert.Equal("first", packet.Body!.GetString());

        CoreTestSupport.SendStreamPacket(client.GetStream(), "second"u8);
        Assert.True(stream.RecvPacket(packet));
        Assert.Equal("second", packet.Body!.GetString());

        packet.Dispose();
        Assert.True(packet.IsEmpty);
        Assert.Null(packet.RoutingId);
        Assert.Null(packet.Header);
        Assert.Null(packet.Body);
    }
}
