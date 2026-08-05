using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkClientServerControlProtocol
{
    private const byte Magic0 = 0x5a;
    private const byte Magic1 = 0x4d;
    private const byte WireMajor = 1;
    private const byte HelloCommand = 1;
    private const byte AdmitCommand = 2;
    private const byte RejectCommand = 3;
    private const byte UpdateCommand = 4;
    private const byte LivenessProbeCommand = 5;
    private const byte LivenessAckCommand = 6;
    private const byte ClientServerTopology = 2;
    private const byte ClientRole = 1;
    private const byte ServerRole = 2;
    private const byte ClientToServerDirection = 1;
    private const int MaximumControlBytes = 1024 * 1024;

    internal sealed record Hello(
        string ChannelName,
        string SecurityIdentity,
        uint NormalizedEffectiveMaxMessageBytes);

    internal sealed record Admission(
        string ChannelName,
        RoutingId ServerRid,
        ulong LifecycleGeneration,
        ulong DescriptorRevision,
        int Weight,
        ZLinkFrameworkRuntimeState State,
        string SecurityIdentity,
        uint NormalizedEffectiveMaxMessageBytes,
        string AdvertisedEndpoint);

    internal static Message EncodeHello(Hello hello)
    {
        var role = new Writer();
        role.Text8(hello.ChannelName);
        role.U8(ClientToServerDirection);
        role.Text8(hello.SecurityIdentity);
        role.NonZeroU32(hello.NormalizedEffectiveMaxMessageBytes);
        return EncodeAdmission(HelloCommand, ClientRole, role);
    }

    internal static Message EncodeAdmission(Admission admission)
        => EncodeServerAdmission(AdmitCommand, admission);

    internal static Message EncodeUpdate(Admission admission)
        => EncodeServerAdmission(UpdateCommand, admission);

    internal static Message EncodeLivenessProbe(ulong probeId) =>
        EncodeLiveness(LivenessProbeCommand, probeId);

    internal static Message EncodeLivenessAck(ulong probeId) =>
        EncodeLiveness(LivenessAckCommand, probeId);

    private static Message EncodeServerAdmission(
        byte command,
        Admission admission)
    {
        if (admission.LifecycleGeneration == 0
            || admission.DescriptorRevision == 0
            || admission.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight)
            throw new ArgumentOutOfRangeException(nameof(admission));
        var role = new Writer();
        role.Text8(admission.ChannelName);
        role.U8(ClientToServerDirection);
        role.Bytes8(admission.ServerRid.ToBytes());
        role.NonZeroU64(admission.LifecycleGeneration);
        role.NonZeroU64(admission.DescriptorRevision);
        role.U32(checked((uint)admission.Weight));
        role.U8(RuntimeStateToWire(admission.State));
        role.Text8(admission.SecurityIdentity);
        role.NonZeroU32(admission.NormalizedEffectiveMaxMessageBytes);
        role.Text16(admission.AdvertisedEndpoint);
        return EncodeAdmission(command, ServerRole, role);
    }

    internal static Message EncodeReject(uint reason)
    {
        if (reason is < 1 or > 12)
            throw new ArgumentOutOfRangeException(nameof(reason));
        var bytes = new byte[9];
        WritePrefix(bytes, RejectCommand);
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(5), reason);
        return Message.From(bytes);
    }

    internal static bool IsControl(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 0)
            return false;
        var span = parts[0].AsReadOnlyMemory().Span;
        return span.Length >= 3
            && span[0] == Magic0
            && span[1] == Magic1
            && span[2] == WireMajor;
    }

    internal static bool TryDecodeHello(
        IReadOnlyList<Message> parts,
        out Hello? hello)
    {
        hello = null;
        if (!TryOpenAdmission(
                parts,
                HelloCommand,
                ClientRole,
                out var reader)
            || !reader.TryText8(out var channelName)
            || !reader.TryU8(out var direction)
            || direction != ClientToServerDirection
            || !reader.TryText8(out var securityIdentity)
            || !reader.TryU32(out var maximum)
            || maximum == 0
            || reader.Remaining != 0)
            return false;
        hello = new Hello(channelName, securityIdentity, maximum);
        return true;
    }

    internal static bool TryDecodeAdmission(
        IReadOnlyList<Message> parts,
        out Admission? admission)
        => TryDecodeServerAdmission(parts, AdmitCommand, out admission);

    internal static bool TryDecodeUpdate(
        IReadOnlyList<Message> parts,
        out Admission? admission)
        => TryDecodeServerAdmission(parts, UpdateCommand, out admission);

    internal static bool TryDecodeLivenessProbe(
        IReadOnlyList<Message> parts,
        out ulong probeId) =>
        TryDecodeLiveness(parts, LivenessProbeCommand, out probeId);

    internal static bool TryDecodeLivenessAck(
        IReadOnlyList<Message> parts,
        out ulong probeId) =>
        TryDecodeLiveness(parts, LivenessAckCommand, out probeId);

    private static bool TryDecodeServerAdmission(
        IReadOnlyList<Message> parts,
        byte command,
        out Admission? admission)
    {
        admission = null;
        if (!TryOpenAdmission(
                parts,
                command,
                ServerRole,
                out var reader)
            || !reader.TryText8(out var channelName)
            || !reader.TryU8(out var direction)
            || direction != ClientToServerDirection
            || !reader.TryBytes8(out var serverRid)
            || !reader.TryU64(out var lifecycle)
            || lifecycle == 0
            || !reader.TryU64(out var revision)
            || revision == 0
            || !reader.TryU32(out var weight)
            || weight > ZLinkSocketConfig.MaximumPeerWeight
            || !reader.TryU8(out var stateValue)
            || !TryRuntimeStateFromWire(stateValue, out var state)
            || !reader.TryText8(out var securityIdentity)
            || !reader.TryU32(out var maximum)
            || maximum == 0
            || !reader.TryText16(out var endpoint)
            || reader.Remaining != 0)
            return false;
        admission = new Admission(
            channelName,
            RoutingId.From(serverRid),
            lifecycle,
            revision,
            checked((int)weight),
            state,
            securityIdentity,
            maximum,
            endpoint);
        return true;
    }

    private static Message EncodeLiveness(byte command, ulong probeId)
    {
        if (probeId is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(probeId));
        var bytes = new byte[13];
        WritePrefix(bytes, command);
        BinaryPrimitives.WriteUInt64BigEndian(bytes.AsSpan(5), probeId);
        return Message.From(bytes);
    }

    private static bool TryDecodeLiveness(
        IReadOnlyList<Message> parts,
        byte command,
        out ulong probeId)
    {
        probeId = 0;
        if (parts.Count != 1)
            return false;
        var span = parts[0].AsReadOnlyMemory().Span;
        if (span.Length != 13 || !HasPrefix(span, command))
            return false;
        probeId = BinaryPrimitives.ReadUInt64BigEndian(span[5..]);
        return probeId is > 0 and <= long.MaxValue;
    }

    internal static bool TryDecodeReject(
        IReadOnlyList<Message> parts,
        out uint reason)
    {
        reason = 0;
        if (parts.Count != 1)
            return false;
        var span = parts[0].AsReadOnlyMemory().Span;
        if (span.Length != 9
            || !HasPrefix(span, RejectCommand)
            || (reason = BinaryPrimitives.ReadUInt32BigEndian(span[5..]))
            is < 1 or > 12)
        {
            reason = 0;
            return false;
        }
        return true;
    }

    internal static uint NormalizeMaximumMessageBytes(long configured)
    {
        if (configured <= 0)
            return uint.MaxValue;
        return checked((uint)Math.Min(configured, uint.MaxValue));
    }

    private static Message EncodeAdmission(
        byte command,
        byte role,
        Writer roleBody)
    {
        if (roleBody.Count > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(roleBody));
        var admissionLength = checked(3 + roleBody.Count);
        var bytes = new byte[checked(10 + admissionLength)];
        WritePrefix(bytes, command);
        bytes[5] = ClientServerTopology;
        BinaryPrimitives.WriteUInt32BigEndian(
            bytes.AsSpan(6),
            checked((uint)admissionLength));
        bytes[10] = role;
        BinaryPrimitives.WriteUInt16BigEndian(
            bytes.AsSpan(11),
            checked((ushort)roleBody.Count));
        roleBody.CopyTo(bytes.AsSpan(13));
        if (bytes.Length > MaximumControlBytes)
            throw new ArgumentOutOfRangeException(nameof(roleBody));
        return Message.From(bytes);
    }

    private static bool TryOpenAdmission(
        IReadOnlyList<Message> parts,
        byte command,
        byte role,
        out Reader reader)
    {
        reader = default;
        if (parts.Count != 1)
            return false;
        var span = parts[0].AsReadOnlyMemory().Span;
        if (span.Length is < 13 or > MaximumControlBytes
            || !HasPrefix(span, command)
            || span[5] != ClientServerTopology)
            return false;
        var admissionLength = BinaryPrimitives.ReadUInt32BigEndian(span[6..]);
        var roleLength = BinaryPrimitives.ReadUInt16BigEndian(span[11..]);
        if (admissionLength != span.Length - 10
            || span[10] != role
            || roleLength != span.Length - 13)
            return false;
        reader = new Reader(span[13..]);
        return true;
    }

    private static bool HasPrefix(ReadOnlySpan<byte> bytes, byte command) =>
        bytes.Length >= 5
        && bytes[0] == Magic0
        && bytes[1] == Magic1
        && bytes[2] == WireMajor
        && bytes[3] == command
        && bytes[4] == 0;

    private static void WritePrefix(Span<byte> bytes, byte command)
    {
        bytes[0] = Magic0;
        bytes[1] = Magic1;
        bytes[2] = WireMajor;
        bytes[3] = command;
        bytes[4] = 0;
    }

    private static byte RuntimeStateToWire(ZLinkFrameworkRuntimeState state) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Preparing => 0,
            ZLinkFrameworkRuntimeState.Serving => 1,
            ZLinkFrameworkRuntimeState.Relocating
                or ZLinkFrameworkRuntimeState.Draining => 2,
            ZLinkFrameworkRuntimeState.Stopped => 3,
            ZLinkFrameworkRuntimeState.Error => 4,
            _ => throw new ArgumentOutOfRangeException(nameof(state))
        };

    private static bool TryRuntimeStateFromWire(
        byte value,
        out ZLinkFrameworkRuntimeState state)
    {
        state = value switch
        {
            0 => ZLinkFrameworkRuntimeState.Preparing,
            1 => ZLinkFrameworkRuntimeState.Serving,
            2 => ZLinkFrameworkRuntimeState.Draining,
            3 => ZLinkFrameworkRuntimeState.Stopped,
            4 => ZLinkFrameworkRuntimeState.Error,
            _ => default
        };
        return value <= 4;
    }

    private sealed class Writer
    {
        private readonly List<byte> _bytes = [];

        internal int Count => _bytes.Count;

        internal void U8(byte value) => _bytes.Add(value);

        internal void U32(uint value)
        {
            Span<byte> bytes = stackalloc byte[sizeof(uint)];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            _bytes.AddRange(bytes);
        }

        internal void NonZeroU32(uint value)
        {
            if (value == 0)
                throw new ArgumentOutOfRangeException(nameof(value));
            U32(value);
        }

        internal void NonZeroU64(ulong value)
        {
            if (value is 0 or > long.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            Span<byte> bytes = stackalloc byte[sizeof(ulong)];
            BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
            _bytes.AddRange(bytes);
        }

        internal void Bytes8(ReadOnlySpan<byte> value)
        {
            if (value.Length is 0 or > byte.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)value.Length));
            _bytes.AddRange(value);
        }

        internal void Text8(string value) =>
            Bytes8(ValidatedUtf8(value, byte.MaxValue));

        internal void Text16(string value)
        {
            var bytes = ValidatedUtf8(value, 4096);
            Span<byte> length = stackalloc byte[sizeof(ushort)];
            BinaryPrimitives.WriteUInt16BigEndian(
                length,
                checked((ushort)bytes.Length));
            _bytes.AddRange(length);
            _bytes.AddRange(bytes);
        }

        internal void CopyTo(Span<byte> destination) =>
            _bytes.CopyTo(destination);

        private static byte[] ValidatedUtf8(string value, int maximum)
        {
            if (string.IsNullOrEmpty(value) || value.Contains('\0'))
                throw new ArgumentException(
                    "Control text must be non-empty and contain no NUL.",
                    nameof(value));
            var bytes = Encoding.UTF8.GetBytes(value);
            if (bytes.Length > maximum)
                throw new ArgumentOutOfRangeException(nameof(value));
            return bytes;
        }
    }

    private ref struct Reader(ReadOnlySpan<byte> bytes)
    {
        private readonly ReadOnlySpan<byte> _bytes = bytes;
        private int _offset;

        internal readonly int Remaining => _bytes.Length - _offset;

        internal bool TryU8(out byte value)
        {
            if (Remaining < 1)
            {
                value = 0;
                return false;
            }
            value = _bytes[_offset++];
            return true;
        }

        internal bool TryU32(out uint value)
        {
            if (Remaining < sizeof(uint))
            {
                value = 0;
                return false;
            }
            value = BinaryPrimitives.ReadUInt32BigEndian(_bytes[_offset..]);
            _offset += sizeof(uint);
            return true;
        }

        internal bool TryU64(out ulong value)
        {
            if (Remaining < sizeof(ulong))
            {
                value = 0;
                return false;
            }
            value = BinaryPrimitives.ReadUInt64BigEndian(_bytes[_offset..]);
            _offset += sizeof(ulong);
            return value <= long.MaxValue;
        }

        internal bool TryBytes8(out ReadOnlySpan<byte> value)
        {
            value = default;
            if (!TryU8(out var length)
                || length == 0
                || Remaining < length)
                return false;
            value = _bytes.Slice(_offset, length);
            _offset += length;
            return true;
        }

        internal bool TryText8(out string value) =>
            TryText(byte.MaxValue, oneByteLength: true, out value);

        internal bool TryText16(out string value) =>
            TryText(4096, oneByteLength: false, out value);

        private bool TryText(
            int maximum,
            bool oneByteLength,
            out string value)
        {
            value = string.Empty;
            int length;
            if (oneByteLength)
            {
                if (!TryU8(out var length8))
                    return false;
                length = length8;
            }
            else
            {
                if (Remaining < sizeof(ushort))
                    return false;
                length = BinaryPrimitives.ReadUInt16BigEndian(
                    _bytes[_offset..]);
                _offset += sizeof(ushort);
            }
            if (length is 0
                || length > maximum
                || Remaining < length)
                return false;
            var text = _bytes.Slice(_offset, length);
            _offset += length;
            if (text.IndexOf((byte)0) >= 0)
                return false;
            try
            {
                value = new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true).GetString(text);
                return true;
            }
            catch (DecoderFallbackException)
            {
                return false;
            }
        }
    }
}
