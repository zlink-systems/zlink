namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamControlFrames
{
    private const string HeartbeatPingName = "$zlink.heartbeat.ping";
    private const string HeartbeatPongName = "$zlink.heartbeat.pong";

    public static bool IsHeartbeatPong(ZlinkStreamHeader header) =>
        header.Kind == ZlinkStreamMessageKind.Control
        && header.Name == HeartbeatPongName;

    public static void SendHeartbeatPing(ZLinkManagedStream stream)
    {
        var ping = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            HeartbeatPingName,
            ZlinkStreamMetadata.Empty);
        ZLinkStreamFrameWriter.Write(
            stream,
            ping,
            ReadOnlySpan<byte>.Empty,
            "Stream heartbeat ping send failed.");
    }

    public static void Dispatch(
        ZLinkManagedStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload)
    {
        if (payload.Length != 0) throw new InvalidOperationException("Stream control packet payload must be empty.");

        if (header.Name == HeartbeatPingName)
        {
            SendHeartbeatPong(stream);
            return;
        }

        if (header.Name == HeartbeatPongName) return;

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
            ZlinkStreamMetadata.Empty);
        ZLinkStreamFrameWriter.Write(
            stream,
            pong,
            ReadOnlySpan<byte>.Empty,
            "Stream heartbeat pong send failed.");
    }
}
