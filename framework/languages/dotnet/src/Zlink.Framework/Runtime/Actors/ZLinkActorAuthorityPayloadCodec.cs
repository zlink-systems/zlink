using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Actors;

internal enum ZLinkActorAuthorityState : byte
{
    Creating = 0,
    Ready = 1
}

internal sealed record ZLinkActorAuthorityPayload(
    ZLinkActorAuthorityState State,
    string StableType,
    string ActorId,
    string CurrentSpotId,
    ulong CurrentSpotGeneration,
    ZLinkSpotKind CurrentSpotKind,
    string OwnerId,
    ulong OwnerLeaseGeneration,
    string MeshName,
    RoutingId NodeRid,
    ulong NodeGeneration);

internal enum ZLinkActorRelocationAuthorityPhase : byte
{
    Activated = 1,
    Cleaning = 2,
    Completed = 3,
    Steady = 4
}

internal sealed record ZLinkActorRelocationAuthorityPayload(
    Guid RelocationId,
    ZLinkActorRelocationAuthorityPhase Phase,
    ZLinkRemoteActorBoundSessionRoute BoundSessionRoute,
    ReadOnlyMemory<byte> ApplicationPayload,
    uint AcceptedRequestCount = 0,
    uint TerminalCompletionCount = 0,
    uint PendingRelayCount = 0);

internal static class ZLinkActorRelocationAuthorityPayloadCodec
{
    private const uint Magic = 0x50414c5a; // ZLAP
    private const ushort Version = 3;
    private const int MaximumBytes = 1024 * 1024;

    internal static byte[] Encode(ZLinkActorRelocationAuthorityPayload value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.RelocationId == Guid.Empty
            || value.Phase is < ZLinkActorRelocationAuthorityPhase.Activated
                or > ZLinkActorRelocationAuthorityPhase.Steady
            || value.ApplicationPayload.Length is < 1 or > MaximumBytes)
            throw new ArgumentOutOfRangeException(nameof(value));

        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write(value.RelocationId.ToByteArray());
        writer.Write((byte)value.Phase);
        writer.Write(value.BoundSessionRoute.IsBound);
        if (value.BoundSessionRoute.IsBound)
        {
            WriteBytes(writer, value.BoundSessionRoute.NodeRid!.Value.ToBytes());
            WriteBytes(writer, value.BoundSessionRoute.SessionRid!.Value.ToBytes());
            WriteString(writer, value.BoundSessionRoute.BindingToken!);
            writer.Write(value.BoundSessionRoute.BindingGeneration);
            writer.Write(value.BoundSessionRoute.ObjectGeneration);
            writer.Write(value.BoundSessionRoute.AuthorityOwnerGeneration);
            WriteString(writer, value.BoundSessionRoute.MeshName!);
            writer.Write(value.BoundSessionRoute.TargetNodeGeneration);
            writer.Write(value.BoundSessionRoute.OwnerLeaseGeneration);
            writer.Write(value.BoundSessionRoute.SessionOwnerNodeGeneration);
            writer.Write(value.BoundSessionRoute.AcceptedHighWater);
        }
        writer.Write(value.ApplicationPayload.Length);
        writer.Write(value.ApplicationPayload.Span);
        writer.Write(value.AcceptedRequestCount);
        writer.Write(value.TerminalCompletionCount);
        writer.Write(value.PendingRelayCount);
        writer.Flush();
        writer.Write(ZLinkCrc32C.Compute(stream.GetBuffer().AsSpan(
            0,
            checked((int)stream.Length))));
        writer.Flush();
        if (stream.Length > MaximumBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        return stream.ToArray();
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorRelocationAuthorityPayload value)
    {
        value = null!;
        try
        {
            if (encoded.Length is < 32 or > MaximumBytes)
                return false;
            var expectedChecksum = BinaryPrimitives.ReadUInt32LittleEndian(
                encoded[^sizeof(uint)..]);
            if (expectedChecksum != ZLinkCrc32C.Compute(encoded[..^sizeof(uint)]))
                return false;
            using var stream = new MemoryStream(
                encoded[..^sizeof(uint)].ToArray(),
                writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
            if (reader.ReadUInt32() != Magic)
                return false;
            var version = reader.ReadUInt16();
            if (version is not (2 or Version)) return false;
            var relocationId = new Guid(ReadExact(reader, 16));
            var phase = (ZLinkActorRelocationAuthorityPhase)reader.ReadByte();
            ZLinkRemoteActorBoundSessionRoute route = default;
            if (reader.ReadBoolean())
            {
                route = new ZLinkRemoteActorBoundSessionRoute(
                    RoutingId.From(ReadBytes(reader)),
                    RoutingId.From(ReadBytes(reader)),
                    ReadString(reader),
                    reader.ReadUInt64(),
                    reader.ReadUInt64(),
                    reader.ReadUInt64(),
                    ReadString(reader),
                    reader.ReadUInt64(),
                    reader.ReadUInt64(),
                    reader.ReadUInt64(),
                    reader.ReadUInt64());
                if (!route.IsBound)
                    return false;
            }
            var applicationSize = reader.ReadInt32();
            if (applicationSize is < 1 or > MaximumBytes)
                return false;
            var applicationPayload = ReadExact(reader, applicationSize);
            var acceptedRequestCount = version >= 3
                ? reader.ReadUInt32()
                : 0;
            var terminalCompletionCount = version >= 3
                ? reader.ReadUInt32()
                : 0;
            var pendingRelayCount = version >= 3
                ? reader.ReadUInt32()
                : 0;
            if (stream.Position != stream.Length
                || relocationId == Guid.Empty
                || phase is < ZLinkActorRelocationAuthorityPhase.Activated
                    or > ZLinkActorRelocationAuthorityPhase.Steady
                || pendingRelayCount > terminalCompletionCount
                || terminalCompletionCount > acceptedRequestCount)
                return false;
            value = new ZLinkActorRelocationAuthorityPayload(
                relocationId,
                phase,
                route,
                applicationPayload,
                acceptedRequestCount,
                terminalCompletionCount,
                pendingRelayCount);
            return true;
        }
        catch (Exception error) when (error is IOException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
    }

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        if (value.Length is < 1 or > byte.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        writer.Write((byte)value.Length);
        writer.Write(value);
    }

    private static byte[] ReadBytes(BinaryReader reader)
        => ReadExact(reader, reader.ReadByte());

    private static void WriteString(BinaryWriter writer, string value)
    {
        var encoded = new UTF8Encoding(false, true).GetBytes(value);
        if (encoded.Length is < 1 or > ushort.MaxValue || value.Contains('\0'))
            throw new ArgumentOutOfRangeException(nameof(value));
        writer.Write((ushort)encoded.Length);
        writer.Write(encoded);
    }

    private static string ReadString(BinaryReader reader)
    {
        var value = new UTF8Encoding(false, true).GetString(
            ReadExact(reader, reader.ReadUInt16()));
        if (value.Length == 0 || value.Contains('\0'))
            throw new InvalidDataException();
        return value;
    }

    private static byte[] ReadExact(BinaryReader reader, int count)
    {
        var value = reader.ReadBytes(count);
        if (value.Length != count)
            throw new EndOfStreamException();
        return value;
    }
}

internal static class ZLinkActorAuthorityPayloadCodec
{
    private static ReadOnlySpan<byte> Magic => "ZLAU"u8;
    private const byte Version = 1;
    private const int MaximumBytes = 1024 * 1024;

    internal static byte[] Encode(ZLinkActorAuthorityPayload value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.CurrentSpotKind is not (ZLinkSpotKind.Entry or ZLinkSpotKind.User))
            throw new ArgumentOutOfRangeException(nameof(value));

        var actor = new Writer();
        actor.Text8(value.StableType);
        actor.Text8(value.ActorId);
        actor.U8((byte)value.State);
        actor.Text8(value.CurrentSpotId);
        actor.U64(value.CurrentSpotGeneration);
        actor.U8((byte)value.CurrentSpotKind);

        var body = new Writer();
        body.U8(value.State switch
        {
            ZLinkActorAuthorityState.Creating => 1,
            ZLinkActorAuthorityState.Ready => 0,
            _ => throw new ArgumentOutOfRangeException(nameof(value))
        });
        body.U8(1);
        body.U16(checked((ushort)actor.Count));
        body.Bytes(actor.ToArray());
        body.Text8(value.OwnerId);
        body.U64(value.OwnerLeaseGeneration);
        body.Text8(value.MeshName);
        body.Rid(value.NodeRid);
        body.U64(value.NodeGeneration);
        body.U8(0);
        body.U32(0);
        body.U8(0);
        body.U32(0);

        var result = new Writer();
        result.Bytes(Magic);
        result.U8(Version);
        result.U16(0);
        result.U32(checked((uint)body.Count));
        result.Bytes(body.ToArray());
        result.U32(ZLinkCrc32C.Compute(result.WrittenSpan));
        var encoded = result.ToArray();
        if (encoded.Length > MaximumBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        return encoded;
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorAuthorityPayload value)
    {
        if (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                encoded,
                out var relocation))
            encoded = relocation.ApplicationPayload.Span;
        if (ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                encoded,
                out var phase))
        {
            if (phase.Phase != ZLinkActorRelocationAuthorityPhase.Steady)
            {
                value = null!;
                return false;
            }
            encoded = phase.ApplicationPayload.Span;
        }
        return TryDecodeDirect(encoded, out value);
    }

    internal static bool TryDecodeRelocating(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorAuthorityPayload value)
    {
        if (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                encoded,
                out var relocation))
            encoded = relocation.ApplicationPayload.Span;
        if (ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                encoded,
                out var phase))
            encoded = phase.ApplicationPayload.Span;
        return TryDecodeDirect(encoded, out value);
    }

    internal static bool TryDecodeDirect(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorAuthorityPayload value)
    {
        value = null!;
        try
        {
            if (encoded.Length > MaximumBytes || encoded.Length < 15
                || !encoded[..4].SequenceEqual(Magic))
                return false;
            var reader = new Reader(encoded);
            reader.Skip(4);
            if (reader.U8() != Version || reader.U16() != 0)
                return false;
            var body = reader.Slice(checked((int)reader.U32()));
            var checksumOffset = reader.Offset;
            if (reader.U32() != ZLinkCrc32C.Compute(encoded[..checksumOffset])
                || !reader.End)
                return false;

            var operation = body.U8();
            if (body.U8() != 1)
                return false;
            var actor = body.Slice(body.U16());
            var stableType = actor.Text8();
            var actorId = actor.Text8();
            var state = (ZLinkActorAuthorityState)actor.U8();
            var currentSpotId = actor.Text8();
            var currentSpotGeneration = actor.U64();
            var currentSpotKind = (ZLinkSpotKind)actor.U8();
            if (!actor.End
                || currentSpotKind is not (ZLinkSpotKind.Entry or ZLinkSpotKind.User)
                || state switch
                {
                    ZLinkActorAuthorityState.Creating => operation != 1,
                    ZLinkActorAuthorityState.Ready => operation != 0,
                    _ => true
                })
                return false;

            var ownerId = body.Text8();
            var ownerLeaseGeneration = body.U64();
            var meshName = body.Text8();
            var nodeRid = body.Rid();
            var nodeGeneration = body.U64();
            if (body.U8() != 0 || body.U32() != 0
                || body.U8() != 0 || body.U32() != 0 || !body.End)
                return false;
            value = new ZLinkActorAuthorityPayload(
                state,
                stableType,
                actorId,
                currentSpotId,
                currentSpotGeneration,
                currentSpotKind,
                ownerId,
                ownerLeaseGeneration,
                meshName,
                nodeRid,
                nodeGeneration);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or OverflowException
                                      or DecoderFallbackException)
        {
            return false;
        }
    }

    internal static ZLinkAuthorityKey AuthorityKey(string actorId)
    {
        var bytes = new UTF8Encoding(false, true).GetBytes(actorId);
        if (bytes.Length is 0 or > byte.MaxValue || actorId.Contains('\0'))
            throw new ArgumentOutOfRangeException(nameof(actorId));
        var builder = new StringBuilder($"zla1:a:{bytes.Length}:");
        foreach (var item in bytes)
        {
            if (item is >= (byte)'A' and <= (byte)'Z'
                or >= (byte)'a' and <= (byte)'z'
                or >= (byte)'0' and <= (byte)'9'
                or (byte)'-' or (byte)'.' or (byte)'_' or (byte)'~')
                builder.Append((char)item);
            else
                builder.Append('%').Append(item.ToString("X2"));
        }
        return new ZLinkAuthorityKey(builder.ToString());
    }

    private sealed class Writer
    {
        private readonly MemoryStream _stream = new();
        internal int Count => checked((int)_stream.Length);
        internal ReadOnlySpan<byte> WrittenSpan => _stream.GetBuffer().AsSpan(0, Count);
        internal void U8(byte value) => _stream.WriteByte(value);
        internal void U16(ushort value)
        {
            Span<byte> bytes = stackalloc byte[2];
            BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void U32(uint value)
        {
            Span<byte> bytes = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void U64(ulong value)
        {
            if (value == 0) throw new ArgumentOutOfRangeException(nameof(value));
            Span<byte> bytes = stackalloc byte[8];
            BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
            _stream.Write(bytes);
        }
        internal void Text8(string value)
        {
            var bytes = new UTF8Encoding(false, true).GetBytes(value);
            if (bytes.Length is 0 or > byte.MaxValue || value.Contains('\0'))
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }
        internal void Rid(RoutingId value)
        {
            var bytes = value.ToBytes();
            if (bytes.Length is 0 or > byte.MaxValue)
                throw new ArgumentOutOfRangeException(nameof(value));
            U8(checked((byte)bytes.Length));
            Bytes(bytes);
        }
        internal void Bytes(ReadOnlySpan<byte> value) => _stream.Write(value);
        internal byte[] ToArray() => _stream.ToArray();
    }

    private ref struct Reader(ReadOnlySpan<byte> bytes)
    {
        private readonly ReadOnlySpan<byte> _bytes = bytes;
        internal int Offset { get; private set; }
        internal bool End => Offset == _bytes.Length;
        internal void Skip(int count)
        {
            Require(count);
            Offset += count;
        }
        internal byte U8()
        {
            Require(1);
            return _bytes[Offset++];
        }
        internal ushort U16()
        {
            Require(2);
            var value = BinaryPrimitives.ReadUInt16BigEndian(_bytes[Offset..]);
            Offset += 2;
            return value;
        }
        internal uint U32()
        {
            Require(4);
            var value = BinaryPrimitives.ReadUInt32BigEndian(_bytes[Offset..]);
            Offset += 4;
            return value;
        }
        internal ulong U64()
        {
            Require(8);
            var value = BinaryPrimitives.ReadUInt64BigEndian(_bytes[Offset..]);
            Offset += 8;
            if (value == 0) throw new InvalidDataException();
            return value;
        }
        internal Reader Slice(int count)
        {
            Require(count);
            var value = new Reader(_bytes.Slice(Offset, count));
            Offset += count;
            return value;
        }
        internal string Text8()
        {
            var length = U8();
            var value = new UTF8Encoding(false, true)
                .GetString(Slice(length)._bytes);
            if (value.Length == 0 || value.Contains('\0'))
                throw new InvalidDataException();
            return value;
        }
        internal RoutingId Rid()
        {
            var length = U8();
            if (length == 0) throw new InvalidDataException();
            return RoutingId.From(Slice(length)._bytes);
        }
        private void Require(int count)
        {
            if (count < 0 || count > _bytes.Length - Offset)
                throw new InvalidDataException();
        }
    }
}
