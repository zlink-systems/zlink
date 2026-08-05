using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorBoundSessionHandoffMetadata
{
    private static ReadOnlySpan<byte> Magic => "ZBSH"u8;
    private const byte Version = 1;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static byte[] Encode(ZLinkActorBoundSessionHandoffFence value)
    {
        ArgumentNullException.ThrowIfNull(value);
        using var stream = new MemoryStream();
        stream.Write(Magic);
        stream.WriteByte(Version);
        WriteText(stream, value.ActorId);
        WriteU64(stream, value.ActorGeneration);
        WriteBytes(stream, value.SessionRid.ToBytes());
        WriteText(stream, value.BindingToken);
        WriteU64(stream, value.BindingGeneration);
        WriteU64(stream, value.SessionSequence);
        return stream.ToArray();
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorBoundSessionHandoffFence value)
    {
        value = null!;
        if (encoded.Length < Magic.Length + 1
            || !encoded[..Magic.Length].SequenceEqual(Magic))
            return false;
        try
        {
            var reader = new Reader(encoded[Magic.Length..]);
            if (reader.Byte() != Version) return false;
            var actorId = reader.Text();
            var actorGeneration = reader.U64();
            var sessionRid = RoutingId.From(reader.Bytes(reader.U16()));
            var bindingToken = reader.Text();
            var bindingGeneration = reader.U64();
            var sessionSequence = reader.U64();
            reader.End();
            if (actorGeneration == 0
                || sessionRid.IsEmpty
                || bindingGeneration == 0
                || sessionSequence == 0)
                return false;
            value = new ZLinkActorBoundSessionHandoffFence(
                actorId,
                actorGeneration,
                sessionRid,
                bindingToken,
                bindingGeneration,
                sessionSequence);
            return true;
        }
        catch (Exception error) when (error is ArgumentException
                                      or EndOfStreamException
                                      or DecoderFallbackException)
        {
            return false;
        }
    }

    private static void WriteText(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        WriteU16(stream, checked((ushort)bytes.Length));
        stream.Write(bytes);
    }

    private static void WriteBytes(Stream stream, ReadOnlySpan<byte> value)
    {
        if (value.Length is < 1 or > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        WriteU16(stream, checked((ushort)value.Length));
        stream.Write(value);
    }

    private static void WriteU16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteU64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private ref struct Reader(ReadOnlySpan<byte> bytes)
    {
        private readonly ReadOnlySpan<byte> _bytes = bytes;
        private int _offset;

        internal byte Byte() => Bytes(1)[0];
        internal ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Bytes(2));
        internal ulong U64() => BinaryPrimitives.ReadUInt64BigEndian(Bytes(8));
        internal string Text() => StrictUtf8.GetString(Bytes(U16()));
        internal ReadOnlySpan<byte> Bytes(int count)
        {
            if (count < 0 || _offset > _bytes.Length - count)
                throw new EndOfStreamException();
            var result = _bytes.Slice(_offset, count);
            _offset += count;
            return result;
        }

        internal void End()
        {
            if (_offset != _bytes.Length) throw new EndOfStreamException();
        }
    }
}
