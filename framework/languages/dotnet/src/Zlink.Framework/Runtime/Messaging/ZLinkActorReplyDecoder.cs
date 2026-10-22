using System.Text.Json;
using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Owns the actor reply wire boundary. Transport details stay here so every
/// public actor request observes the framework error family.
/// </summary>
internal static class ZLinkActorReplyDecoder
{
    // captureFlow threads the live diagnostics gate into the reply ingress
    // decode (spec 27 §4): an Off host neither reads nor validates the
    // observation-only flow fields on the terminal reply.
    public static TReply Decode<TReply>(IReadOnlyList<Message> reply, bool captureFlow = true)
    {
        var payload = DecodeResponsePayload(reply, captureFlow);
        try
        {
            return ZLinkFrameworkJsonPayloadCodec.Deserialize<TReply>(payload)
                ?? throw ProtocolError("Actor request reply payload is null.");
        }
        catch (Exception error)
            when (error is not OperationCanceledException and not ZLinkFrameworkException)
        {
            throw ProtocolError("Actor request reply payload could not be decoded.", error);
        }
        finally
        {
            GC.KeepAlive(reply);
        }
    }

    public static void DecodeTerminal(IReadOnlyList<Message> reply, bool captureFlow = true)
    {
        _ = DecodeResponsePayload(reply, captureFlow);
        GC.KeepAlive(reply);
    }

    private static ReadOnlySpan<byte> DecodeResponsePayload(
        IReadOnlyList<Message> reply,
        bool captureFlow
    )
    {
        if (reply.Count != 1)
            throw ProtocolError("Actor request reply must contain one STREAM frame.");

        ZlinkStreamHeader header;
        ReadOnlySpan<byte> payload;
        try
        {
            var frame = reply[0].AsReadOnlySpan();
            if (!ZLinkStreamFrameCodec.TryDecode(frame, out var headerBytes, out payload))
                throw new InvalidOperationException("Actor request reply frame is invalid.");
            // The header decoder requires managed memory. Copy only that
            // header; the reply retains the native payload until the caller
            // finishes this synchronous decode.
            header = ZLinkStreamProtocolDefaults.DecodeHeader(headerBytes.ToArray(), captureFlow);
        }
        catch (Exception error)
            when (error is not OperationCanceledException and not ZLinkFrameworkException)
        {
            throw ProtocolError("Actor request reply header is malformed.", error);
        }

        if (header.Kind == ZlinkStreamMessageKind.Error)
            throw DecodeError(payload);
        if (header.Kind != ZlinkStreamMessageKind.Response)
            throw ProtocolError($"Actor request reply kind '{header.Kind}' is not a response.");

        return payload;
    }

    private static Exception DecodeError(ReadOnlySpan<byte> payload)
    {
        ZLinkStreamWireError? error;
        try
        {
            error = JsonSerializer.Deserialize<ZLinkStreamWireError>(
                payload,
                ZLinkJsonSerializerOptions.Default
            );
        }
        catch (Exception decodeError) when (decodeError is not OperationCanceledException)
        {
            return ProtocolError("Actor request error payload could not be decoded.", decodeError);
        }

        if (error is null)
            return ProtocolError("Actor request error payload is null.");
        // Cross-language snake_case wire names only (C++ stream_error_code);
        // an unknown name stays a protocol error.
        if (ZLinkErrorWireNames.TryParse(error.Code, out var kind))
            return new ZLinkFrameworkException(kind, error.Message ?? "Actor request failed.");

        return ProtocolError(
            $"Actor request reply uses an unknown error code '{error.Code ?? "<missing>"}': "
                + (error.Message ?? "Actor request failed.")
        );
    }

    private static ZLinkFrameworkException ProtocolError(
        string message,
        Exception? innerException = null
    ) => new(ZLinkFrameworkErrorKind.ProtocolError, message, innerException: innerException);
}
