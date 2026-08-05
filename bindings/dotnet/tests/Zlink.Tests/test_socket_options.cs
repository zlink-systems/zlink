using System;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_options
{
    [Fact]
    public void socket_options_runtime_int_and_long_roundtrip()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        using var dealer = ctx.CreateDealerSocket();

        stream.Options.Notify = false;
        Assert.False(stream.Options.Notify);

        stream.Options.MaxMessageSize = 1024L;
        Assert.Equal(1024L, stream.Options.MaxMessageSize);

        RoutingId dealerRid = CoreTestSupport.RoutingIdUtf8("RID-OPT");
        dealer.SetRoutingId(dealerRid);
        Assert.Equal(dealerRid, dealer.GetRoutingId());
    }

    [Fact]
    public void byte_high_water_marks_roundtrip_64_bit_boundaries()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        ctx.Options.AutoHwmEnabled = false;
        using var pair = ctx.CreatePairSocket();

        Assert.Equal(4_096_000UL, pair.Options.SendHighWaterMark);
        Assert.Equal(4_096_000UL, pair.Options.ReceiveHighWaterMark);

        const ulong beyondInt32 = (ulong)int.MaxValue + 65_536UL;
        pair.Options.SendHighWaterMark = beyondInt32;
        pair.Options.ReceiveHighWaterMark = ulong.MaxValue;

        Assert.Equal(beyondInt32, pair.Options.SendHighWaterMark);
        Assert.Equal(ulong.MaxValue, pair.Options.ReceiveHighWaterMark);

        pair.Options.SendHighWaterMark = 0UL;
        pair.Options.ReceiveHighWaterMark = 0UL;
        Assert.Equal(0UL, pair.Options.SendHighWaterMark);
        Assert.Equal(0UL, pair.Options.ReceiveHighWaterMark);
    }

    [Fact]
    public void high_water_mark_public_properties_reject_32_bit_shape()
    {
        var send = typeof(CommonSocketOptions).GetProperty(
            nameof(CommonSocketOptions.SendHighWaterMark));
        var receive = typeof(CommonSocketOptions).GetProperty(
            nameof(CommonSocketOptions.ReceiveHighWaterMark));

        Assert.Equal(typeof(ulong), send?.PropertyType);
        Assert.Equal(typeof(ulong), receive?.PropertyType);
    }

    [Fact]
    public void submit_retry_mode_values_match_native_contract()
    {
        Assert.Equal(0, (int)SubmitRetryMode.Off);
        Assert.Equal(1, (int)SubmitRetryMode.LocalFailure);
    }

    [Fact]
    public void submit_retry_options_roundtrip_and_validate_native_bounds()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();

        Assert.Equal(SubmitRetryMode.Off, router.Options.SubmitRetryMode);
        Assert.Equal(0, router.Options.SubmitRetryTimeoutMilliseconds);
        Assert.Equal(0, router.Options.SubmitRetryAttempts);

        router.Options.SubmitRetryMode = SubmitRetryMode.LocalFailure;
        router.Options.SubmitRetryTimeoutMilliseconds = 250;
        router.Options.SubmitRetryAttempts = 16;

        Assert.Equal(SubmitRetryMode.LocalFailure,
            router.Options.SubmitRetryMode);
        Assert.Equal(250, router.Options.SubmitRetryTimeoutMilliseconds);
        Assert.Equal(16, router.Options.SubmitRetryAttempts);

        Assert.Throws<ZlinkConfigException>(() =>
            router.Options.SubmitRetryMode = (SubmitRetryMode)2);
        Assert.Throws<ZlinkConfigException>(() =>
            router.Options.SubmitRetryTimeoutMilliseconds = -1);
        Assert.Throws<ZlinkConfigException>(() =>
            router.Options.SubmitRetryAttempts = 17);
    }

    [Fact]
    public void socket_options_runtime_string_getter_works()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "socket-options-last-endpoint");
        router.Bind(endpoint);

        string actual = router.Options.LastEndpoint;
        Assert.StartsWith("tcp://", actual);
    }

    [Fact]
    public void typed_socket_option_helpers_route_to_supported_options()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        using var stream = ctx.CreateStreamSocket();
        using var xpub = ctx.CreateXPubSocket();

        RoutingId dealerRid = CoreTestSupport.RoutingIdUtf8("DEALER-RID");
        dealer.SetRoutingId(dealerRid);
        Assert.Equal(dealerRid, dealer.GetRoutingId());

        RoutingId routerRid = CoreTestSupport.RoutingIdUtf8("ROUTER-RID");
        router.SetRoutingId(routerRid);
        Assert.Equal(routerRid, router.GetRoutingId());
        router.Options.Mandatory = true;
        Assert.True(router.Options.Mandatory);

        stream.Options.Notify = true;
        Assert.True(stream.Options.Notify);

        xpub.Options.Verbose = true;
        xpub.Options.Verboser = true;
        xpub.Options.NoDrop = true;
    }

    [Fact]
    public void xpub_welcome_message_preserves_binary_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var xpub = ctx.CreateXPubSocket();
        byte[] payload = [0x00, 0x41, 0xFF, 0x42, 0x00];

        using Message welcome = Message.From(payload);
        xpub.Options.WelcomeMessage = welcome;

        using Message actual = xpub.Options.WelcomeMessage;
        Assert.Equal(payload, actual.ToArray());
    }

    [Fact]
    public void peer_weight_options_validate_documented_range()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();

        Assert.Equal(100, dealer.Options.PeerWeight);
        dealer.Options.PeerWeight = 0;
        Assert.Equal(0, dealer.Options.PeerWeight);
        dealer.Options.PeerWeight = 100;
        Assert.Equal(100, dealer.Options.PeerWeight);
        router.Options.PeerWeight = 0;
        Assert.Equal(0, router.Options.PeerWeight);
        router.Options.PeerWeight = 100;
        Assert.Equal(100, router.Options.PeerWeight);

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            dealer.Options.PeerWeight = -1);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            dealer.Options.PeerWeight = 101);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            router.Options.PeerWeight = -1);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            router.Options.PeerWeight = 101);
    }

    [Fact]
    public void subscription_at_returns_null_when_index_is_absent()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sub = ctx.CreateSubSocket();

        Assert.Null(sub.SubscriptionAt(0));

        sub.SetSubscription("prices");

        Assert.Equal("prices", sub.SubscriptionAt(0)?.Filter);
    }

}
