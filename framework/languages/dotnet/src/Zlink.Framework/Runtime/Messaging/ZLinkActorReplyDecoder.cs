using System.Text.Json;
using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Owns the actor reply wire boundary. Transport details stay here so every
/// public actor request observes the framework error family.
/// </summary>
internal static class ZLinkActorReplyDecoder
{
    public static TReply Decode<TReply>(IReadOnlyList<Message> reply)
    {
        if (reply.Count == 0)
            throw ProtocolError("Actor request reply is empty.");

        ZlinkStreamHeader header;
        ReadOnlySpan<byte> payload;
        try
        {
            if (reply.Count == 1)
            {
                var frame = reply[0].AsReadOnlySpan();
                if (!ZLinkStreamFrameCodec.TryDecode(frame, out var headerBytes, out payload))
                    throw new InvalidOperationException("Actor request reply frame is invalid.");
                header = ZLinkStreamProtocolDefaults.DecodeHeader(headerBytes.ToArray());
            }
            else
            {
                header = ZLinkStreamProtocolDefaults.DecodeHeader(reply[0].AsReadOnlyMemory());
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
            return JsonSerializer.Deserialize<TReply>(payload, ZLinkJsonSerializerOptions.Default)
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
        if (Enum.TryParse<ZLinkFrameworkErrorKind>(error.Code, out var kind)
            && Enum.IsDefined(kind))
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
