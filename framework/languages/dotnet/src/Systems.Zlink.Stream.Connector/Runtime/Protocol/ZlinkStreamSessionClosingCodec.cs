using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamSessionClosingCodec
{
    public const string ControlName = "session-closing";
    private const int PrefixSize = sizeof(byte) + sizeof(byte) + sizeof(ushort);
    private const int DiagnosticLengthOffset = sizeof(byte) + sizeof(byte);
    private const byte WireReasonOffset = 1;
    private const byte Version = 1;
    private const int MaximumDiagnosticBytes = 512;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static byte[] EncodeServerDrain(string? diagnostic = null) =>
        Encode(ZlinkStreamCloseReason.ServerDrain, diagnostic);

    public static byte[] EncodeIdleTimeout(string? diagnostic = null) =>
        Encode(ZlinkStreamCloseReason.IdleTimeout, diagnostic);

    public static byte[] EncodeHeartbeatTimeout(string? diagnostic = null) =>
        Encode(ZlinkStreamCloseReason.HeartbeatTimeout, diagnostic);

    public static byte[] EncodeProtocolError(string? diagnostic = null) =>
        Encode(ZlinkStreamCloseReason.ProtocolError, diagnostic);

    public static ZlinkStreamHeader CreateHeader() =>
        new(
            ZlinkStreamMessageKind.Control,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            ControlName,
            ZlinkStreamMetadata.Empty
        );

    public static ZlinkStreamSessionClosing Decode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < PrefixSize)
            throw Error("Session-closing payload is truncated.");
        if (payload[0] != Version)
            throw Error("Session-closing version is not supported.");

        var reason = payload[1] switch
        {
            (byte)ZlinkStreamCloseReason.ClientClose + WireReasonOffset =>
                ZlinkStreamCloseReason.ClientClose,
            (byte)ZlinkStreamCloseReason.IdleTimeout + WireReasonOffset =>
                ZlinkStreamCloseReason.IdleTimeout,
            (byte)ZlinkStreamCloseReason.HeartbeatTimeout + WireReasonOffset =>
                ZlinkStreamCloseReason.HeartbeatTimeout,
            (byte)ZlinkStreamCloseReason.ServerDrain + WireReasonOffset =>
                ZlinkStreamCloseReason.ServerDrain,
            (byte)ZlinkStreamCloseReason.ProtocolError + WireReasonOffset =>
                ZlinkStreamCloseReason.ProtocolError,
            (byte)ZlinkStreamCloseReason.TransportError + WireReasonOffset =>
                ZlinkStreamCloseReason.TransportError,
            _ => throw Error("Session-closing reason is not supported."),
        };
        var diagnosticLength = BinaryPrimitives.ReadUInt16BigEndian(
            payload.Slice(DiagnosticLengthOffset, sizeof(ushort))
        );
        if (diagnosticLength > MaximumDiagnosticBytes)
            throw Error("Session-closing diagnostic is too large.");
        if (payload.Length != PrefixSize + diagnosticLength)
            throw Error("Session-closing diagnostic length does not match the payload.");

        try
        {
            var diagnostic =
                diagnosticLength == 0
                    ? null
                    : StrictUtf8.GetString(payload.Slice(PrefixSize, diagnosticLength));
            return new ZlinkStreamSessionClosing(reason, diagnostic);
        }
        catch (DecoderFallbackException exception)
        {
            throw Error("Session-closing diagnostic is not valid UTF-8.", exception);
        }
    }

    private static byte[] Encode(ZlinkStreamCloseReason reason, string? diagnostic)
    {
        int diagnosticLength;
        try
        {
            diagnosticLength = diagnostic is null ? 0 : StrictUtf8.GetByteCount(diagnostic);
        }
        catch (EncoderFallbackException exception)
        {
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Session-closing diagnostic is not valid UTF-8.",
                exception
            );
        }
        if (diagnosticLength > MaximumDiagnosticBytes)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                $"Session-closing diagnostic must not exceed {MaximumDiagnosticBytes} UTF-8 bytes."
            );

        var payload = new byte[PrefixSize + diagnosticLength];
        payload[0] = Version;
        payload[1] = checked((byte)((byte)reason + WireReasonOffset));
        BinaryPrimitives.WriteUInt16BigEndian(
            payload.AsSpan(DiagnosticLengthOffset, sizeof(ushort)),
            checked((ushort)diagnosticLength)
        );
        if (diagnosticLength > 0)
            StrictUtf8.GetBytes(diagnostic!, payload.AsSpan(PrefixSize));
        return payload;
    }

    private static ZlinkStreamException Error(string message, Exception? exception = null) =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, message, exception);
}

internal readonly record struct ZlinkStreamSessionClosing(
    ZlinkStreamCloseReason Reason,
    string? Diagnostic
);
