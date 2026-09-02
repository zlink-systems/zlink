using System.Net;
using System.Net.Sockets;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_stream_socket
{
    [Fact]
    public void receive_mode_rejects_unspecified_and_is_fixed_after_bind()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            stream.Options.ReceiveMode = StreamReceiveMode.Unspecified);
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        stream.Bind(CoreTestSupport.NewEndpoint("tcp", "stream-mode"));
        Assert.Throws<ZlinkConfigException>(() =>
            stream.Options.ReceiveMode = StreamReceiveMode.Raw);
    }

    [Fact]
    public void packet_receive_decodes_header_body_and_source_route()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-packet");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);
        using var client = new TcpClient();
        client.Connect(IPAddress.Loopback, port);
        CoreTestSupport.SendStreamPacket(client.GetStream(), "packet"u8);

        using var packet = StreamPacket.Create();
        Assert.True(stream.RecvPacket(packet));
        Assert.NotNull(packet.RoutingId);
        Assert.NotNull(packet.Header);
        Assert.Equal("packet", packet.Body!.GetString());
    }

    [Fact]
    public async Task packet_source_route_can_be_used_by_unified_send()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-reply");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);
        using var client = new TcpClient();
        client.ReceiveTimeout = 5000;
        client.Connect(IPAddress.Loopback, port);
        CoreTestSupport.SendStreamPacket(client.GetStream(), "request"u8);

        using var packet = StreamPacket.Create();
        Assert.True(stream.RecvPacket(packet));
        using Message reply = Message.From("reply");
        await stream.Send(packet.RoutingId!.Value).Message(reply).Async();

        byte[] echoed = new byte[5];
        int read = client.GetStream().Read(echoed);
        Assert.Equal(5, read);
        Assert.Equal("reply", System.Text.Encoding.UTF8.GetString(echoed));
    }

    [Fact]
    public void packet_dontwait_leaves_empty_output_empty()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Options.ReceiveMode = StreamReceiveMode.Packet;
        stream.Bind(CoreTestSupport.NewEndpoint("tcp", "stream-empty"));
        using var packet = StreamPacket.Create();
        Assert.False(stream.RecvPacket(packet, RecvFlags.DontWait));
        Assert.True(packet.IsEmpty);
    }
}
