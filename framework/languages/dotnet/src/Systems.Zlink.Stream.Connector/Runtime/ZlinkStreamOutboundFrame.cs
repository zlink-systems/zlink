namespace Systems.Zlink.Stream.Connector.Runtime;

internal readonly record struct ZlinkStreamOutboundFrame(
    ReadOnlyMemory<byte> HeaderBytes,
    ReadOnlyMemory<byte> PayloadBytes,
    ReadOnlyMemory<byte> EncodedBytes = default,
    Action? ValidateWrite = null
);
