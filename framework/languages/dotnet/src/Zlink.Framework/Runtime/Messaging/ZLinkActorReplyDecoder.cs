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
    public static TReply Decode<TReply>(
        IReadOnlyList<Message> reply,
        bool captureFlow = true)
    {
        if (reply.Count == 0)
            throw ProtocolError("Actor request reply is empty.");

        ZlinkStreamHeader header;
        ReadOnlySpan<byte> payload;
        try
        {
            if (reply.Count == 1)
            {
                var frameMemory = reply[0].AsReadOnlyMemory();
                if (!ZLinkStreamFrameCodec.TryDecode(
                        frameMemory.Span, out var headerBytes, out payload))
                    throw new InvalidOperationException("Actor request reply frame is invalid.");
                // The header always sits at PrefixSize inside the frame, so
                // re-slicing the frame memory decodes without copying the
                // header bytes out first.
                header = ZLinkStreamProtocolDefaults.DecodeHeader(
                    frameMemory.Slice(
                        ZLinkStreamFrameCodec.PrefixSize, headerBytes.Length),
                    captureFlow);
            }
            else
            {
                header = ZLinkStreamProtocolDefaults.DecodeHeader(
                    reply[0].AsReadOnlyMemory(),
                    captureFlow);
                payload = reply[1].AsReadOnlySpan();
            }
        }
        catch (Exception error) when (error is not OperationCanceledException
                                      and not ZLinkFrameworkException)
        {
            throw ProtocolError("Actor request reply header is malformed.", error);
        }

        if (header.Kind == ZlinkStreamMessageKind.Error)
            throw DecodeError(payload);
        if (header.Kind != ZlinkStreamMessageKind.Response)
            throw ProtocolError($"Actor request reply kind '{header.Kind}' is not a response.");

        try
        {
            return ZLinkFrameworkJsonPayloadCodec.Deserialize<TReply>(payload)
                   ?? throw PayloadDecodeFailed("Actor request reply payload is null.");
        }
        catch (Exception error) when (error is not OperationCanceledException
                                      and not ZLinkFrameworkException)
        {
            throw PayloadDecodeFailed("Actor request reply payload could not be decoded.", error);
        }
    }

    private static Exception DecodeError(ReadOnlySpan<byte> payload)
    {
        ZLinkStreamWireError? error;
        try
        {
            error = JsonSerializer.Deserialize<ZLinkStreamWireError>(
                payload,
                ZLinkJsonSerializerOptions.Default);
        }
        catch (Exception decodeError) when (decodeError is not OperationCanceledException)
        {
            return PayloadDecodeFailed("Actor request error payload could not be decoded.", decodeError);
        }

        if (error is null)
            return PayloadDecodeFailed("Actor request error payload is null.");
        // Cross-language snake_case wire names only (C++ stream_error_code);
        // an unknown name stays a protocol error.
        if (ZLinkErrorWireNames.TryParse(error.Code, out var kind))
            return new ZLinkFrameworkException(kind, error.Message ?? "Actor request failed.");

        return ProtocolError(
            $"Actor request reply uses an unknown error code '{error.Code ?? "<missing>"}': "
            + (error.Message ?? "Actor request failed."));
    }

    private static ZLinkFrameworkException ProtocolError(
        string message,
        Exception? innerException = null) => new(
        ZLinkFrameworkErrorKind.ProtocolError,
        message,
        innerException: innerException);

    private static ZLinkFrameworkException PayloadDecodeFailed(
        string message,
        Exception? innerException = null) => new(
        ZLinkFrameworkErrorKind.ProtocolError,
        message,
        innerException: innerException);
}
