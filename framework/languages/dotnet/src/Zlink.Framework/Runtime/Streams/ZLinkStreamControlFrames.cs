namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamControlFrames
{
    private const string ActorBoundName = "$zlink.actor.bound";
    private const string ActorUnboundName = "$zlink.actor.unbound";
    private const string HeartbeatPingName = "$zlink.heartbeat.ping";
    private const string HeartbeatPongName = "$zlink.heartbeat.pong";

    public static bool IsHeartbeatPong(ZlinkStreamHeader header) =>
        header.Kind == ZlinkStreamMessageKind.Control && header.Name == HeartbeatPongName;

    public static void SendHeartbeatPing(ZLinkManagedStream stream)
    {
        var ping = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            HeartbeatPingName,
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
        var payload = new byte[4 + actorIdBytes.Length];
        payload[0] = 1;
        System.Buffers.Binary.BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(1, 2), slot);
        payload[3] = checked((byte)actorIdBytes.Length);
        actorIdBytes.CopyTo(payload.AsSpan(4));
        SendControl(stream, ActorBoundName, payload);
    }

    public static void SendActorUnbound(IZLinkStream stream, ushort slot)
    {
        if (slot == 0)
            throw new InvalidOperationException("Actor slot must not be zero.");
        Span<byte> payload = stackalloc byte[3];
        payload[0] = 1;
        System.Buffers.Binary.BinaryPrimitives.WriteUInt16BigEndian(payload.Slice(1, 2), slot);
        SendControl(stream, ActorUnboundName, payload);
    }

    public static void Dispatch(
        ZLinkManagedStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload
    )
    {
        if (payload.Length != 0)
            throw new InvalidOperationException("Stream control packet payload must be empty.");

        if (header.Name == HeartbeatPingName)
        {
            SendHeartbeatPong(stream);
            return;
        }

        if (header.Name == HeartbeatPongName)
            return;

        throw new InvalidOperationException("Unknown stream control packet.");
    }

    private static void SendHeartbeatPong(ZLinkManagedStream stream)
    {
        var pong = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            HeartbeatPongName,
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
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
        if (!stream.Write(ZLinkMessage.From(frame)))
            throw new InvalidOperationException("Actor control packet send failed.");
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
