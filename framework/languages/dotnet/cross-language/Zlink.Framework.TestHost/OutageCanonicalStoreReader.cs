using System.Buffers.Binary;
using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Systems.Zlink;
using Zlink.Framework.LocationProvider;

// Location runtime canonical Store records and service-wire-v1 authority payload.
// Read only fields asserted by the outage scenario; never call runtime codecs.
internal static class OutageCanonicalStoreReader
{
    internal sealed record Authority(
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration,
        string OwnerId,
        long OwnerLeaseGeneration,
        byte[] Payload
    );

    internal sealed record Relocation(
        string SourceOwnerId,
        ulong SourceOwnerLeaseGeneration,
        string CoordinatorOwnerId,
        ulong CoordinatorLeaseGeneration
    );

    internal static ZLinkStoreKey ActorKey(string actorId) => new($"authority\0actor\0{actorId}");

    internal static ZLinkStoreKey OwnerKey(string ownerId) => new($"owner-lease\0{ownerId}");

    internal static ZLinkStoreKey MeshKey(string meshName, RoutingId rid) =>
        new($"mesh-node\0{meshName}\0{rid.ToHex()}");

    private static JsonDocument Record(ReadOnlyMemory<byte> bytes)
    {
        var document = JsonDocument.Parse(bytes);
        if (document.RootElement.GetProperty("recordVersion").GetInt32() != 1)
        {
            document.Dispose();
            throw new InvalidDataException("Unknown canonical Store record version.");
        }
        return document;
    }

    private static ulong Unsigned(JsonElement record, string field) =>
        ulong.Parse(record.GetProperty(field).GetString()!, CultureInfo.InvariantCulture);

    internal static Authority ReadAuthority(ReadOnlyMemory<byte> bytes)
    {
        using var document = Record(bytes);
        var record = document.RootElement;
        return new(
            Unsigned(record, "objectGeneration"),
            Unsigned(record, "authorityOwnerGeneration"),
            record.GetProperty("ownerId").GetString()!,
            checked((long)Unsigned(record, "ownerLeaseGeneration")),
            record.GetProperty("payload").GetBytesFromBase64()
        );
    }

    internal static long ReadOwnerLease(ReadOnlyMemory<byte> bytes)
    {
        using var document = Record(bytes);
        return checked((long)Unsigned(document.RootElement, "leaseGeneration"));
    }

    internal static (string OwnerId, long LeaseGeneration) ReadDescriptorOwner(
        ReadOnlyMemory<byte> bytes
    )
    {
        using var document = Record(bytes);
        var descriptor = document.RootElement.GetProperty("descriptor");
        var owner = descriptor.GetProperty("ownerId").GetString()!;
        var lease = Unsigned(descriptor, "leaseGeneration");
        if (
            owner != document.RootElement.GetProperty("ownerId").GetString()
            || lease != Unsigned(document.RootElement, "leaseGeneration")
        )
            throw new InvalidDataException("Canonical descriptor owner fields disagree.");
        return (owner, checked((long)lease));
    }

    internal static bool IsSteadyReady(byte[] payload)
    {
        var body = AuthorityBody(payload);
        var operation = body.U8();
        var kind = body.U8();
        var actor = new Reader(body.Take(body.U16()));
        _ = actor.Text8(); // actorType
        _ = actor.Text8(); // actorId
        var state = actor.U8();
        _ = actor.Text8(); // currentSpotId
        actor.Skip(8); // currentSpotGeneration
        var spotKind = actor.U8();
        _ = body.Text8(); // ownerId
        body.Skip(8); // ownerLeaseGeneration
        _ = body.Text8(); // ownerMeshName
        _ = body.Text8(); // ownerNodeRid
        body.Skip(8); // ownerNodeGeneration
        return operation == 0
            && kind == 1
            && state == 1
            && actor.End
            && spotKind is 1 or 2
            && body.U8() == 0
            && body.U32() == 0 // no relocation state
            && body.U8() == 0
            && body.U32() == 0 // no activation recovery state
            && body.End;
    }

    private static readonly Lazy<MethodInfo> ReadAuthorityAggregate = new(() =>
        typeof(Zlink.Framework.Contracts.Locations.ZLinkLocationOptions)
            .Assembly.GetType(
                "Zlink.Framework.Runtime.Locations.ZLinkAuthorityAggregateCodec",
                throwOnError: true
            )!
            .GetMethod("Read", BindingFlags.Static | BindingFlags.NonPublic)!
    );

    internal static Relocation? ReadRelocation(ReadOnlyMemory<byte> bytes)
    {
        using var document = Record(bytes);
        if (
            !document.RootElement.TryGetProperty("aggregate", out var aggregate)
            || aggregate.ValueKind == JsonValueKind.Null
        )
            return null;

        var relocation = ReadAuthorityAggregate.Value.Invoke(null, [aggregate]);
        if (relocation is null)
            return null;

        var relocationType = relocation.GetType();
        var source = relocationType.GetProperty("Source")!.GetValue(relocation)!;
        var coordinator = relocationType.GetProperty("Coordinator")!.GetValue(relocation)!;
        return new(
            (string)source.GetType().GetProperty("OwnerId")!.GetValue(source)!,
            (ulong)source.GetType().GetProperty("OwnerLeaseGeneration")!.GetValue(source)!,
            (string)coordinator.GetType().GetProperty("OwnerId")!.GetValue(coordinator)!,
            (ulong)coordinator.GetType().GetProperty("OwnerLeaseGeneration")!.GetValue(coordinator)!
        );
    }

    private static Reader AuthorityBody(byte[] payload)
    {
        var envelope = new Reader(payload);
        if (
            !envelope.Take(4).AsSpan().SequenceEqual("ZLAU"u8)
            || envelope.U8() != 1
            || envelope.U16() != 0
        )
            throw new InvalidDataException("Unknown canonical authority envelope.");
        var body = envelope.Take(checked((int)envelope.U32()));
        var checksum = envelope.U32();
        if (!envelope.End || checksum != Crc32C(payload.AsSpan(0, payload.Length - 4)))
            throw new InvalidDataException("Invalid canonical authority checksum or length.");
        return new Reader(body);
    }

    private static uint Crc32C(ReadOnlySpan<byte> bytes)
    {
        var crc = uint.MaxValue;
        foreach (var value in bytes)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ ((crc & 1) == 0 ? 0 : 0x82f63b78u);
        }
        return ~crc;
    }

    private sealed class Reader(byte[] bytes)
    {
        private int _offset;
        internal bool End => _offset == bytes.Length;

        internal byte[] Take(int count)
        {
            if (count < 0 || count > bytes.Length - _offset)
                throw new InvalidDataException("Truncated canonical authority payload.");
            var result = bytes.AsSpan(_offset, count).ToArray();
            _offset += count;
            return result;
        }

        internal void Skip(int count) => _ = Take(count);

        internal byte U8() => Take(1)[0];

        internal ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Take(2));

        internal uint U32() => BinaryPrimitives.ReadUInt32BigEndian(Take(4));

        internal string Text8() => new UTF8Encoding(false, true).GetString(Take(U8()));
    }
}
