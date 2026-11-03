using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamControlFrames
{
    public static void SendHeartbeatPing(ZLinkManagedStream stream)
    {
        var ping = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            ZlinkStreamControlProtocol.HeartbeatPingName,
            ZlinkStreamMetadata.Empty
        );
        ZLinkStreamFrameWriter.Write(
            stream,
            ping,
            ReadOnlySpan<byte>.Empty,
            "Stream heartbeat ping send failed."
        );
    }

    public static void SendActorBound(IZLinkStream stream, ushort slot, string actorId)
    {
        var actorIdBytes = System.Text.Encoding.UTF8.GetBytes(actorId);
        if (slot == 0 || actorIdBytes.Length is 0 or > byte.MaxValue)
            throw new InvalidOperationException("Actor binding control values are invalid.");
        var payload = new byte[
            ZlinkStreamControlProtocol.ActorBoundPrefixSize + actorIdBytes.Length
        ];
        payload[0] = ZlinkStreamControlProtocol.ActorControlVersion;
        System.Buffers.Binary.BinaryPrimitives.WriteUInt16BigEndian(
            payload.AsSpan(
                ZlinkStreamControlProtocol.ActorSlotOffset,
                ZlinkStreamControlProtocol.ActorSlotSize
            ),
            slot
        );
        payload[ZlinkStreamControlProtocol.ActorIdLengthOffset] = checked(
            (byte)actorIdBytes.Length
        );
        actorIdBytes.CopyTo(payload.AsSpan(ZlinkStreamControlProtocol.ActorBoundPrefixSize));
        SendControl(stream, ZlinkStreamControlProtocol.BoundControlName, payload);
    }

    public static async ValueTask SendActorUnboundAsync(
        IZLinkStream stream,
        ushort slot,
        CancellationToken cancellationToken
    )
    {
        if (slot == 0)
            throw new InvalidOperationException("Actor slot must not be zero.");
        var payload = new byte[ZlinkStreamControlProtocol.ActorUnboundPayloadSize];
        payload[0] = ZlinkStreamControlProtocol.ActorControlVersion;
        System.Buffers.Binary.BinaryPrimitives.WriteUInt16BigEndian(
            payload.AsSpan(
                ZlinkStreamControlProtocol.ActorSlotOffset,
                ZlinkStreamControlProtocol.ActorSlotSize
            ),
            slot
        );
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            ZlinkStreamControlProtocol.UnboundControlName,
            ZlinkStreamMetadata.Empty
        );
        await ZLinkStreamFrameWriter
            .WriteAsync(stream, header, payload, cancellationToken)
            .ConfigureAwait(false);
    }

    public static void Dispatch(
        ZLinkManagedStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        ILogger logger
    )
    {
        if (header.Name == ZlinkStreamControlProtocol.HeartbeatPingName)
        {
            if (payload.Length != 0)
                LogIgnoredControl(stream, header.Name, payload.Length, logger);
            SendHeartbeatPong(stream);
            return;
        }

        if (header.Name == ZlinkStreamControlProtocol.HeartbeatPongName)
        {
            if (payload.Length != 0)
                LogIgnoredControl(stream, header.Name, payload.Length, logger);
            return;
        }

        if (
            header.Name == ZlinkStreamControlProtocol.BoundControlName
            || header.Name == ZlinkStreamControlProtocol.UnboundControlName
        )
            throw new InvalidOperationException("Unexpected stream actor control packet.");

        if (header.Name.StartsWith("$zlink.", StringComparison.Ordinal))
        {
            LogIgnoredControl(stream, header.Name, payload.Length, logger);
            return;
        }

        throw new InvalidOperationException("Unknown stream control packet.");
    }

    private static void LogIgnoredControl(
        ZLinkManagedStream stream,
        string name,
        int payloadLength,
        ILogger logger
    )
    {
        logger.LogDebug(
            "Ignored STREAM control name={ControlName} payloadBytes={PayloadBytes} peerSessionRid={PeerSessionRid}",
            name,
            payloadLength,
            stream.RoutingId?.ToString()
        );
    }

    private static void SendHeartbeatPong(ZLinkManagedStream stream)
    {
        var pong = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            ZlinkStreamControlProtocol.HeartbeatPongName,
            ZlinkStreamMetadata.Empty
        );
        ZLinkStreamFrameWriter.Write(
            stream,
            pong,
            ReadOnlySpan<byte>.Empty,
            "Stream heartbeat pong send failed."
        );
    }

    private static void SendControl(IZLinkStream stream, string name, ReadOnlySpan<byte> payload)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty
        );
        ZLinkStreamFrameWriter.Write(stream, header, payload, "Actor control packet send failed.");
    }
}

internal static class ZLinkStreamActorFrames
{
    internal static byte[] WithActorSlot(byte[] frame, ushort slot)
    {
        if (!ZLinkStreamFrameCodec.TryDecode(frame, out var headerBytes, out var payload))
            throw new InvalidOperationException("Actor STREAM frame is invalid.");
        var header = ZLinkStreamProtocolDefaults.DecodeHeader(headerBytes.ToArray()) with
        {
            ActorSlot = slot,
        };
        return ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
    }
}
