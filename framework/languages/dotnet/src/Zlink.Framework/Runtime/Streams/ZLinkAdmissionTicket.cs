using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkAdmissionTicket
{
    private const int ClockSkewMs = 2000;
    private const int MinKeyBytes = 32;
    private const int MaxKeyBytes = 64;
    internal const int LifetimeMs = 30000;
    private static readonly int MinKeyEncodedSize = ZlinkAdmissionTicketWire.UnpaddedBase64UrlSize(
        MinKeyBytes
    );
    private static readonly int MaxKeyEncodedSize = ZlinkAdmissionTicketWire.UnpaddedBase64UrlSize(
        MaxKeyBytes
    );
    private static readonly UTF8Encoding Utf8 = new(false, true);

    private readonly byte[] _current;
    private readonly byte[]? _previous;

    internal ZLinkAdmissionTicket(string? current, string? previous)
    {
        _current = ReadKey(current, "AdmissionQueueKey");
        _previous = previous is null ? null : ReadKey(previous, "AdmissionQueuePreviousKey");
    }

    internal string Sign(AdmissionConnection connection, ulong epoch, long expiresAt)
    {
        Span<byte> ticket = stackalloc byte[ZlinkAdmissionTicketWire.Size];
        ticket[0] = ZlinkAdmissionTicketWire.Version;
        BinaryPrimitives.WriteInt64BigEndian(ticket[1..], expiresAt);
        BinaryPrimitives.WriteUInt64BigEndian(ticket[9..], epoch);
        WriteMac(
            _current,
            connection,
            ticket[1..ZlinkAdmissionTicketWire.HeaderSize],
            ticket[ZlinkAdmissionTicketWire.HeaderSize..]
        );
        return Encode(ticket);
    }

    internal bool Verify(string? value, AdmissionConnection connection, ulong epoch, long now)
    {
        if (
            epoch == 0
            || value?.Length != ZlinkAdmissionTicketWire.EncodedSize
            || !TryDecode(value, out var ticket)
            || ticket.Length != ZlinkAdmissionTicketWire.Size
            || ticket[0] != ZlinkAdmissionTicketWire.Version
            || BinaryPrimitives.ReadUInt64BigEndian(ticket.AsSpan(9)) != epoch
            || now - ClockSkewMs > BinaryPrimitives.ReadInt64BigEndian(ticket.AsSpan(1))
        )
            return false;

        Span<byte> mac = stackalloc byte[ZlinkAdmissionTicketWire.MacSize];
        var signedFields = ticket.AsSpan(1, ZlinkAdmissionTicketWire.HeaderSize - 1);
        var expected = ticket.AsSpan(ZlinkAdmissionTicketWire.HeaderSize);
        WriteMac(_current, connection, signedFields, mac);
        if (CryptographicOperations.FixedTimeEquals(mac, expected))
            return true;
        if (_previous is null)
            return false;
        WriteMac(_previous, connection, signedFields, mac);
        return CryptographicOperations.FixedTimeEquals(mac, expected);
    }

    internal static bool IsStreamName(string? name)
    {
        try
        {
            return name is not null && Utf8.GetByteCount(name) is >= 1 and <= byte.MaxValue;
        }
        catch (EncoderFallbackException)
        {
            return false;
        }
    }

    internal static string Encode(ReadOnlySpan<byte> bytes) =>
        Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    internal static bool TryDecode(string? value, out byte[] bytes)
    {
        bytes = [];
        if (string.IsNullOrEmpty(value) || value.Length % 4 == 1)
            return false;
        var base64 = value.Replace('-', '+').Replace('_', '/');
        try
        {
            bytes = Convert.FromBase64String(base64.PadRight((base64.Length + 3) / 4 * 4, '='));
        }
        catch (FormatException)
        {
            return false;
        }
        return string.Equals(Encode(bytes), value, StringComparison.Ordinal);
    }

    private static byte[] ReadKey(string? value, string name)
    {
        if (
            value is not null
            && value.Length >= MinKeyEncodedSize
            && value.Length <= MaxKeyEncodedSize
            && TryDecode(value, out var bytes)
            && bytes.Length is >= MinKeyBytes and <= MaxKeyBytes
        )
            return bytes;
        throw new ZLinkConfigurationException(
            $"{name} must be unpadded base64url encoding of {MinKeyBytes} through {MaxKeyBytes} bytes."
        );
    }

    private static void WriteMac(
        byte[] key,
        AdmissionConnection connection,
        ReadOnlySpan<byte> signedFields,
        Span<byte> mac
    )
    {
        TryDecode(connection.NodeRid, out var nodeRid);
        TryDecode(connection.SessionRid, out var sessionRid);
        ReadOnlySpan<byte> domain = "zlink-admission-ticket-v1\0"u8;
        Span<byte> input = stackalloc byte[domain.Length + 3 * (1 + byte.MaxValue) + 8 + 16];
        domain.CopyTo(input);
        var offset = domain.Length;
        input[offset++] = checked((byte)nodeRid.Length);
        nodeRid.CopyTo(input[offset..]);
        offset += nodeRid.Length;
        BinaryPrimitives.WriteUInt64BigEndian(input[offset..], connection.NodeLifecycleGeneration);
        offset += sizeof(ulong);
        var streamLength = Utf8.GetBytes(connection.StreamName, input[(offset + 1)..]);
        input[offset++] = checked((byte)streamLength);
        offset += streamLength;
        input[offset++] = checked((byte)sessionRid.Length);
        sessionRid.CopyTo(input[offset..]);
        offset += sessionRid.Length;
        signedFields.CopyTo(input[offset..]);
        HMACSHA256.HashData(key, input[..(offset + signedFields.Length)], mac);
    }
}
