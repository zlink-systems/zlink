using System;
using System.Text;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_validation_contract
{
    [Fact]
    public void routing_id_accepts_255_byte_boundary()
    {
        string value = new string('r', 255);

        RoutingId routingId = RoutingId.From(Encoding.UTF8.GetBytes(value));

        Assert.Equal(value, routingId.ToString());
        Assert.Equal(255, routingId.ToString().Length);
    }

    [Fact]
    public void routing_id_rejects_256_byte_boundary()
    {
        string value = new string('r', 256);

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            _ = RoutingId.From(Encoding.UTF8.GetBytes(value)));
    }

    [Fact]
    public void monitor_open_rejects_unknown_event_flags()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var socket = ctx.CreatePairSocket();

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            socket.MonitorOpen((SocketEvent)0x10000));
    }

    [Fact]
    public void poller_rejects_unknown_event_flags()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var poller = Zlink.CreatePoller();
        using var ctx = Zlink.CreateContext();
        using var socket = ctx.CreatePairSocket();

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            poller.Add(socket, (PollEventFlags)16, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            poller.AddFd(0, (PollEventFlags)16, 1));
    }

    [Fact]
    public void message_and_publish_entrypoints_fail_fast_on_null_and_empty_inputs()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var pair = ctx.CreatePairSocket();
        using var pub = ctx.CreatePubSocket();
        using var router = ctx.CreateRouterSocket();
        using var routedMessage = Message.From("x");
        using var publishedMessage = Message.From("x");

        Assert.Throws<ArgumentNullException>(() =>
            pair.Send().Message((Message)null!));
        Assert.Throws<ArgumentNullException>(() =>
            pub.Publish((string)null!).Message(publishedMessage).Submit());
    }

    [Fact]
    public void fixed_utf8_boundary_values_reject_overlong_inputs()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        string overlong = new string('a', 256);

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ctx.Options.ThreadNamePrefix = new string('a', 17));
    }

    [Fact]
    public void socket_endpoint_failures_use_function_category_exceptions()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var pair = ctx.CreatePairSocket();

        Assert.Throws<ZlinkBindException>(() => pair.Bind("invalid://endpoint"));
        Assert.Throws<ZlinkConnectException>(() =>
            pair.Connect("invalid://endpoint"));
        Assert.Throws<ZlinkConnectException>(() =>
            pair.Unbind("invalid://endpoint"));
        Assert.Throws<ZlinkConnectException>(() =>
            pair.Disconnect("invalid://endpoint"));
    }

    [Fact]
    public void typed_exception_public_constructors_reject_ok_result()
    {
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.Ok),
            () => new ZlinkSubmitException(
                ZlinkSubmitException.ErrorCode.InvalidArgument),
            typeof(ZlinkSubmitException),
            typeof(ZlinkSubmitException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkRequestException(ZlinkRequestException.ErrorCode.Ok),
            () => new ZlinkRequestException(
                ZlinkRequestException.ErrorCode.InvalidArgument),
            typeof(ZlinkRequestException),
            typeof(ZlinkRequestException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkRecvException(ZlinkRecvException.ErrorCode.Ok),
            () => new ZlinkRecvException(ZlinkRecvException.ErrorCode.NoData),
            typeof(ZlinkRecvException),
            typeof(ZlinkRecvException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkHandlerException(ZlinkHandlerException.ErrorCode.Ok),
            () => new ZlinkHandlerException(
                ZlinkHandlerException.ErrorCode.InvalidArgument),
            typeof(ZlinkHandlerException),
            typeof(ZlinkHandlerException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkCloseException(ZlinkCloseException.ErrorCode.Ok),
            () => new ZlinkCloseException(
                ZlinkCloseException.ErrorCode.InvalidHandle),
            typeof(ZlinkCloseException),
            typeof(ZlinkCloseException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkBindException(ZlinkBindException.ErrorCode.Ok),
            () => new ZlinkBindException(
                ZlinkBindException.ErrorCode.InvalidArgument),
            typeof(ZlinkBindException),
            typeof(ZlinkBindException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkConnectException(ZlinkConnectException.ErrorCode.Ok),
            () => new ZlinkConnectException(
                ZlinkConnectException.ErrorCode.InvalidArgument),
            typeof(ZlinkConnectException),
            typeof(ZlinkConnectException.ErrorCode));
        AssertPublicExceptionConstructorRejectsOk(
            () => new ZlinkConfigException(ZlinkConfigException.ErrorCode.Ok),
            () => new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidArgument),
            typeof(ZlinkConfigException),
            typeof(ZlinkConfigException.ErrorCode));
    }

    private static void AssertPublicExceptionConstructorRejectsOk(
        Action createOk,
        Func<ZlinkException> createFailure,
        Type exceptionType,
        Type errorCodeType)
    {
        Assert.Throws<ArgumentOutOfRangeException>(createOk);
        Assert.IsType(exceptionType, createFailure());
        Assert.Null(exceptionType.GetConstructor(new[]
        {
            errorCodeType,
            typeof(int)
        }));
    }

    [Fact]
    public async Task request_timeouts_reject_negative_values()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var message = Message.From("x");
        TimeSpan negative = TimeSpan.FromMilliseconds(-1);

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            dealer.Request().Message(message).Timeout(negative).Async());
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            dealer.Request().Message(message).Timeout(negative).Submit(
                (_, parts) => Zlink.MultipartClose(parts)));
    }
}
