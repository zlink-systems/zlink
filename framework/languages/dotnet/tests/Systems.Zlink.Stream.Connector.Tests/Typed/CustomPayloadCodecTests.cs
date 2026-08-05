using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task CustomPayloadCodecEncodesAndDecodesTypedConnectorCalls()
    {
        var codec = new CustomTextCodec();
        var connector = new RecordingConnector(codec);

        // Send: the custom codec produces the wire payload, not the built-in JSON auto-codec.
        await connector.Send(new CustomProbe("hello"))
            .PacketName("custom.send").Async();
        Assert.Equal(ZlinkStreamCodec.Raw, connector.SendCall.Payload.Codec);
        Assert.Equal("CUSTOM:hello", Encoding.UTF8.GetString(connector.SendCall.Payload.Payload.Span));

        // Request: reply body is decoded with the custom codec.
        connector.NextReply = codec.Encode(new CustomProbe("reply"));
        var reply = await connector.Request(new CustomProbe("ask"))
            .PacketName("custom.request")
            .Async<CustomProbe>();
        Assert.Equal("reply", reply.Text);
        Assert.Equal("CUSTOM:ask", Encoding.UTF8.GetString(connector.RequestCall.Payload.Payload.Span));

        // Submit callback path also decodes with the custom codec.
        ZlinkStreamResult<CustomProbe>? callbackResult = null;
        connector.NextCallbackPayloadResult =
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(codec.Encode(new CustomProbe("callback")));
        connector.Request(new CustomProbe("ask"))
            .Submit<CustomProbe>(result => callbackResult = result);
        Assert.True(callbackResult?.IsSuccess);
        Assert.Equal("callback", callbackResult.GetValueOrDefault().Value!.Text);

        // On handler decodes inbound notifications with the custom codec.
        CustomProbe? handlerPayload = null;
        using var subscription = connector.On<CustomProbe>("custom.notify",
            (message, _) =>
            {
                handlerPayload = message.Payload;
                return ValueTask.CompletedTask;
            });
        await connector.InvokeHandler("custom.notify", codec.Encode(new CustomProbe("pushed")));
        Assert.Equal("pushed", handlerPayload?.Text);

        // WaitFor decodes with the custom codec, including the Where filter.
        connector.RecordReceived("custom.wait", codec.Encode(new CustomProbe("first")));
        connector.RecordReceived("custom.wait", codec.Encode(new CustomProbe("second")));
        var filtered = await connector.WaitFor<CustomProbe>("custom.wait")
            .Where(message => message.Payload.Text == "second")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        Assert.Equal("second", filtered.Payload.Text);
    }

    [Fact]
    public void AutoCodecFallsBackToBuiltInWhenNoCustomCodecIsRegistered()
    {
        // Without a custom codec the typed API keeps the built-in JSON auto-selection.
        var connector = new RecordingConnector();

        connector.Send(new CustomProbe("plain"))
            .PacketName("auto.json").Async();

        Assert.Equal(ZlinkStreamCodec.Json, connector.SendCall.Payload.Codec);
    }

    private sealed record CustomProbe(string Text);

    private sealed class CustomTextCodec : IZlinkStreamPayloadCodec
    {
        public ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload)
        {
            var probe = (CustomProbe)(object)payload!;
            return new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Raw,
                Encoding.UTF8.GetBytes("CUSTOM:" + probe.Text));
        }

        public TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload)
        {
            var text = Encoding.UTF8.GetString(payload.Payload.Span);
            var value = text.StartsWith("CUSTOM:", StringComparison.Ordinal)
                ? text["CUSTOM:".Length..]
                : text;
            return (TPayload)(object)new CustomProbe(value);
        }
    }
}
