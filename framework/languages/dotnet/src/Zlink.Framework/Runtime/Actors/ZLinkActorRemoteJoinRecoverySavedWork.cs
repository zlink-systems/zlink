using System.Buffers.Binary;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>Canonical saved-work representation for a pending routed Actor join.</summary>
internal static class ZLinkActorRemoteJoinRecoverySavedWork
{
    // Reserved only for frozen saved-work: the kind-1 payload is byte-exact
    // ZLJR (request, terminal reply and metadata), never an application packet.
    internal const string PacketName = "__zlink.actor.routed_join.recovery";
    private const string ContentType = "application/x-zlink-actor-routed-join-recovery-v1";
    private static readonly UTF8Encoding Utf8 = new(false, true);

    internal static ZLinkRelocationQueuedJob Create(
        ulong order, ZLinkActorRelocationSourceFence source, ReadOnlyMemory<byte> zlir)
    {
        _ = ZLinkActorRemoteJoinRecoveryCodec.Decode(zlir.Span);
        using var stream = new MemoryStream();
        stream.WriteByte(1); // canonical frozen record: node application send
        stream.WriteByte(1); // node source
        using (var identity = new MemoryStream())
        {
            Text8(identity, source.NodeRid.ToHex());
            U64(identity, source.NodeGeneration);
            Text8(identity, source.OwnerId);
            U64(identity, source.OwnerLeaseGeneration);
            U16(stream, checked((ushort)identity.Length));
            identity.Position = 0;
            identity.CopyTo(stream);
        }
        stream.WriteByte(0); // no application metadata
        U64(stream, 0); U64(stream, 0); U32(stream, 0); U16(stream, 0);
        using var payload = new MemoryStream();
        Text8(payload, PacketName); Text8(payload, ContentType);
        U32(payload, checked((uint)zlir.Length)); payload.Write(zlir.Span);
        stream.WriteByte(1); U32(stream, checked((uint)payload.Length));
        payload.Position = 0; payload.CopyTo(stream);
        return new ZLinkRelocationQueuedJob(order, stream.ToArray());
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorRelocationSourceFence source,
        out ZLinkActorRelocationRecoveryRecord recovery)
    {
        source = default!; recovery = default!;
        try
        {
            var r = new Reader(encoded);
            if (r.U8() != 1 || r.U8() != 1) return false;
            var s = new Reader(r.Bytes(r.U16()));
            var rid = RoutingId.From(s.Text8()); var generation = s.U64();
            var owner = s.Text8(); var lease = s.U64(); s.End();
            if (r.U8() != 0 || r.U64() != 0 || r.U64() != 0 || r.U32() != 0 || r.U16() != 0
                || r.U8() != 1) return false;
            var p = new Reader(r.Bytes(checked((int)r.U32())));
            if (p.Text8() != PacketName || p.Text8() != ContentType) return false;
            recovery = ZLinkActorRemoteJoinRecoveryCodec.Decode(p.Bytes(checked((int)p.U32())));
            p.End(); r.End();
            source = new ZLinkActorRelocationSourceFence(owner, lease, rid, generation);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException or EndOfStreamException or ArgumentException or OverflowException)
        { return false; }
    }

    private static void Text8(Stream s, string value) { var b = Utf8.GetBytes(value); if (b.Length is < 1 or > byte.MaxValue || b.AsSpan().Contains((byte)0)) throw new ArgumentOutOfRangeException(nameof(value)); s.WriteByte((byte)b.Length); s.Write(b); }
    private static void U16(Stream s, ushort v) { Span<byte> b = stackalloc byte[2]; BinaryPrimitives.WriteUInt16BigEndian(b, v); s.Write(b); }
    private static void U32(Stream s, uint v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(b, v); s.Write(b); }
    private static void U64(Stream s, ulong v) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteUInt64BigEndian(b, v); s.Write(b); }
    private ref struct Reader(ReadOnlySpan<byte> b) { private readonly ReadOnlySpan<byte> _b = b; private int _o; internal byte U8()=>Bytes(1)[0]; internal ushort U16()=>BinaryPrimitives.ReadUInt16BigEndian(Bytes(2)); internal uint U32()=>BinaryPrimitives.ReadUInt32BigEndian(Bytes(4)); internal ulong U64()=>BinaryPrimitives.ReadUInt64BigEndian(Bytes(8)); internal string Text8()=>Utf8.GetString(Bytes(U8())); internal ReadOnlySpan<byte> Bytes(int n) { if (n < 0 || _o > _b.Length-n) throw new EndOfStreamException(); var x=_b.Slice(_o,n); _o+=n; return x; } internal void End(){if(_o!=_b.Length) throw new InvalidDataException();} }
}
