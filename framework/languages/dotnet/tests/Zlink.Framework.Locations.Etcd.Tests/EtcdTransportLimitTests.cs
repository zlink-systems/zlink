using Etcdserverpb;
using Google.Protobuf;

namespace Zlink.Framework.Locations.Etcd.Tests;

// Spec 07 §9.2 client bounds, checked at the exact serialized message size.
public sealed class EtcdTransportLimitTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public async Task Send_limit_accepts_exactly_16_MiB_and_fails_one_byte_more_before_dispatch(
        int excess
    )
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var password = PasswordForRequestSize(EtcdClient.MaxSendMessageBytes + excess);
        await using var store = server.CreateStore(
            credentials: new() { User = "u", Password = password }
        );
        if (excess == 0)
        {
            Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("absent")));
            Assert.Equal(1, server.AuthenticateCount);
            Assert.Single(server.Calls, call => call.Method == "Txn");
        }
        else
        {
            await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("absent")).AsTask());
            Assert.Equal(0, server.AuthenticateCount);
            Assert.Empty(server.Calls);
        }
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public async Task Receive_limit_accepts_exactly_8_MiB_and_fails_one_byte_more(int excess)
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var size = EtcdClient.MaxReceiveMessageBytes + excess;
        server.AuthenticateReply = response => Padded(response, size);
        await using var store = server.CreateStore(
            credentials: new() { User = "u", Password = "p" }
        );
        if (excess == 0)
        {
            Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("absent")));
            Assert.Single(server.Calls, call => call.Method == "Txn");
        }
        else
        {
            await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("absent")).AsTask());
            Assert.DoesNotContain(server.Calls, call => call.Method == "Txn");
        }
        Assert.Equal(1, server.AuthenticateCount);
    }

    [Fact]
    public void Channels_take_no_service_config_and_use_the_spec_bounds()
    {
        using var handler = new SocketsHttpHandler();
        var options = EtcdClient.ChannelOptions(handler);
        Assert.Null(options.ServiceConfig);
        Assert.True(options.DisableResolverServiceConfig);
        Assert.Equal(EtcdClient.MaxSendMessageBytes, options.MaxSendMessageSize);
        Assert.Equal(EtcdClient.MaxReceiveMessageBytes, options.MaxReceiveMessageSize);
    }

    private static string PasswordForRequestSize(int size)
    {
        // Both password lengths need a four-byte varint, so the field overhead is the same.
        var probe = new AuthenticateRequest { Name = "u", Password = new string('p', size) };
        var password = new string('p', 2 * size - probe.CalculateSize());
        var request = new AuthenticateRequest { Name = "u", Password = password };
        Assert.Equal(size, request.CalculateSize());
        return password;
    }

    private static AuthenticateResponse Padded(AuthenticateResponse response, int size)
    {
        var overhead = WithUnknownField(response, size).CalculateSize() - size;
        var padded = WithUnknownField(response, size - overhead);
        Assert.Equal(size, padded.CalculateSize());
        return padded;
    }

    // Field 1000 is unknown to AuthenticateResponse: it adds bytes the client skips after
    // its size check, so the token stays short.
    private static AuthenticateResponse WithUnknownField(AuthenticateResponse response, int length)
    {
        using var stream = new MemoryStream();
        using (var output = new CodedOutputStream(stream, leaveOpen: true))
        {
            response.WriteTo(output);
            output.WriteTag(1000, WireFormat.WireType.LengthDelimited);
            output.WriteBytes(ByteString.CopyFrom(new byte[length]));
        }
        return AuthenticateResponse.Parser.ParseFrom(stream.ToArray());
    }
}
