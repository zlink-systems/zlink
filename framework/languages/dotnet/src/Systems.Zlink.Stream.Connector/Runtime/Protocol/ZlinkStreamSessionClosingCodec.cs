using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamSessionClosingCodec
{
    public const string ControlName = "session-closing";
    private const byte Version = 1;
    private const int MaximumDiagnosticBytes = 512;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static ZlinkStreamSessionClosing Decode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 4)
            throw Error("Session-closing payload is truncated.");
        if (payload[0] != Version)
            throw Error("Session-closing version is not supported.");

        var reason = payload[1] switch
        {
            1 => ZlinkStreamCloseReason.ClientClose,
            2 => ZlinkStreamCloseReason.IdleTimeout,
            3 => ZlinkStreamCloseReason.HeartbeatTimeout,
            4 => ZlinkStreamCloseReason.ServerDrain,
            5 => ZlinkStreamCloseReason.ProtocolError,
            6 => ZlinkStreamCloseReason.TransportError,
            _ => throw Error("Session-closing reason is not supported.")
        };
        var diagnosticLength = BinaryPrimitives.ReadUInt16BigEndian(payload.Slice(2, 2));
        if (diagnosticLength > MaximumDiagnosticBytes)
            throw Error("Session-closing diagnostic is too large.");
        if (payload.Length != 4 + diagnosticLength)
            throw Error("Session-closing diagnostic length does not match the payload.");

        try
        {
            var diagnostic = diagnosticLength == 0
                ? null
                : StrictUtf8.GetString(payload.Slice(4, diagnosticLength));
            return new ZlinkStreamSessionClosing(reason, diagnostic);
        }
        catch (DecoderFallbackException exception)
        {
            throw Error("Session-closing diagnostic is not valid UTF-8.", exception);
        }
    }

    private static ZlinkStreamException Error(string message, Exception? exception = null) =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, message, exception);
}

internal readonly record struct ZlinkStreamSessionClosing(
    ZlinkStreamCloseReason Reason,
    string? Diagnostic);
