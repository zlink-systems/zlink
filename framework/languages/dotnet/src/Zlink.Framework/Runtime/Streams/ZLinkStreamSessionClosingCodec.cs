using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamSessionClosingCodec
{
    public const string ControlName = "session-closing";
    private const byte Version = 1;
    private const byte IdleTimeoutReason = 2;
    private const byte HeartbeatTimeoutReason = 3;
    private const byte ServerDrainReason = 4;
    private const byte ProtocolErrorReason = 5;
    private const int MaximumDiagnosticBytes = 512;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static byte[] EncodeServerDrain(string? diagnostic = null)
        => Encode(ServerDrainReason, diagnostic);

    public static byte[] EncodeIdleTimeout(string? diagnostic = null)
        => Encode(IdleTimeoutReason, diagnostic);

    public static byte[] EncodeHeartbeatTimeout(string? diagnostic = null)
        => Encode(HeartbeatTimeoutReason, diagnostic);

    public static byte[] EncodeProtocolError(string? diagnostic = null)
        => Encode(ProtocolErrorReason, diagnostic);

    private static byte[] Encode(byte reason, string? diagnostic)
    {
        var length = diagnostic is null ? 0 : StrictUtf8.GetByteCount(diagnostic);
        if (length > MaximumDiagnosticBytes)
            throw new ArgumentOutOfRangeException(
                nameof(diagnostic),
                "Session-closing diagnostic must not exceed 512 UTF-8 bytes.");

        var payload = new byte[4 + length];
        payload[0] = Version;
        payload[1] = reason;
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(2, 2), (ushort)length);
        if (length > 0) StrictUtf8.GetBytes(diagnostic!, payload.AsSpan(4));
        return payload;
    }

    public static ZlinkStreamHeader CreateHeader() => new(
        ZlinkStreamMessageKind.Control,
        ZlinkStreamCodec.Raw,
        ZlinkStreamHeaderFlags.None,
        null,
        ControlName,
        ZlinkStreamMetadata.Empty);
}
