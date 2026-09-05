using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_hot_path_ownership_contract
{
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public async Task repeated_part_preserves_each_wire_part_and_consumes_source_once(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "scratch-alias");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using Message source = Message.From("shared-part");
        var operation = sender.Send().Message(source);
        for (var i = 1; i < count; i++)
            operation.Message(source);
        await operation.Async();
        Assert.Throws<ObjectDisposedException>(() => source.GetString());
        using Received received = Received.Create();
        Assert.True(receiver.Recv(received));
        Assert.Equal(count, received.Parts.Count);
        Assert.All(received.Parts, part => Assert.Equal("shared-part", part.GetString()));
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public void invalid_last_part_preserves_prefix_and_does_not_stage_it(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var sender = context.CreateDealerSocket();
        using var receiver = context.CreateDealerSocket();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "scratch-invalid");
        receiver.Bind(endpoint);
        sender.Connect(endpoint);
        using Message source = Message.From("prefix");
        using Message invalid = Message.From("invalid");
        invalid.Dispose();
        var operation = sender.Send().Message(source);
        for (var i = 1; i < count - 1; i++)
            operation.Message(source);
        operation.Message(invalid);
        Assert.Throws<ObjectDisposedException>(() => operation.TrySubmit());
        Assert.Equal("prefix", source.GetString());
        sender.Send().Message(source).Submit();
        using Received received = Received.Create();
        Assert.True(receiver.Recv(received));
        Assert.Equal("prefix", received.SinglePartOrThrow().GetString());
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    [InlineData(33)]
    public void native_final_failure_preserves_all_originals(int count)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Options.Mandatory = true;
        using Message source = Message.From("preserved");
        var operation = router.Send(RoutingId.From("missing"u8)).Message(source);
        for (var i = 1; i < count; i++)
            operation.Message(source);
        var error = Assert.Throws<ZlinkSubmitException>(() => operation.TrySubmit());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
        Assert.Equal("preserved", source.GetString());
    }
}
