using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkAggregateInventory
{
    internal const int MaxPageBytes = 1024 * 1024;
    private const int MaxMembershipMutationBytes = 1024 * 1024;
    internal static readonly ReadOnlyMemory<byte> NoMembershipMutation = "[]"u8.ToArray();
    private const int MaxPageItems = 1024;
    private const int MaxTreeLevels = 32;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly JsonSerializerOptions CanonicalJson = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new CanonicalStringConverter() },
    };

    // Record properties are declared in RFC 8785 order. All numeric fields are Int32.
    internal sealed record Entry(
        string AuthorityKey,
        string AuthorityPayloadSha256,
        string ExpectedStoreVersion,
        int Index,
        string MembershipMutationSha256,
        string OwnerTransition
    );

    internal sealed record Reference(
        int EntryCount,
        int Index,
        int Level,
        string Sha256,
        int StartIndex
    );

    internal sealed record Page(
        Reference[] Children,
        Entry[] Entries,
        int EntryCount,
        int Index,
        int Level,
        int StartIndex
    );

    internal sealed record Root(
        string DigestSha256,
        int TopLevel,
        Reference[] TopPages,
        int TotalCount
    );

    internal sealed record Participant(
        Entry Entry,
        ReadOnlyMemory<byte> Payload,
        ReadOnlyMemory<byte> Mutation
    );

    internal sealed record Tree(Root Root, IReadOnlyList<(Page Page, byte[] Bytes)> Pages);

    internal sealed record Aggregate(
        string AggregateGeneration,
        string AggregateId,
        AggregateCapacity Capacity,
        string InventoryDigestSha256,
        int ParticipantCount,
        string State,
        AggregateDescriptor TargetDescriptor,
        string TargetDescriptorLifecycleGeneration,
        AggregateOwner TargetOwner
    );

    internal sealed record AggregateCapacity(int Actors, AggregateSpotType? SpotType, int Spots);

    internal sealed record AggregateSpotType(int Count, string ObjectKind, string StableType);

    internal sealed record AggregateDescriptor(string MeshName, string RoutingIdHex);

    internal sealed record AggregateOwner(string OwnerId, string OwnerLeaseGeneration);

    internal static ZLinkAggregateFence RelocationFence(Guid relocationId) => new(relocationId, 1);

    internal static ZLinkStoreKey AggregateKey(string prefix, ZLinkAggregateFence fence) =>
        new(
            FormattableString.Invariant(
                $"{prefix}aggregate:{fence.AggregateId:D}:{fence.AggregateGeneration}"
            )
        );

    internal static Aggregate Describe(
        ZLinkAggregateFence fence,
        string state,
        int participantCount,
        string digest,
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong descriptorLifecycleGeneration,
        ZLinkCapacityVector capacity,
        ZLinkLocationOwnerToken owner
    ) =>
        new(
            fence.AggregateGeneration.ToString(System.Globalization.CultureInfo.InvariantCulture),
            fence.AggregateId.ToString("D"),
            new(
                capacity.Actors,
                capacity.SpotType is { } spotType
                    ? new(
                        spotType.Count,
                        spotType.ObjectKind switch
                        {
                            ZLinkPlacementObjectKind.UserSpot => "userSpot",
                            ZLinkPlacementObjectKind.InstanceSpot => "instanceSpot",
                            _ => throw new ArgumentOutOfRangeException(nameof(capacity)),
                        },
                        spotType.StableType
                    )
                    : null,
                capacity.Spots
            ),
            digest,
            participantCount,
            state,
            new(descriptor.MeshName, descriptor.Rid.ToHex()),
            descriptorLifecycleGeneration.ToString(
                System.Globalization.CultureInfo.InvariantCulture
            ),
            new(
                owner.OwnerId,
                owner.LeaseGeneration.ToString(System.Globalization.CultureInfo.InvariantCulture)
            )
        );

    internal static ZLinkStoreKey RootKey(string prefix, ZLinkAggregateFence fence) =>
        new(KeyPrefix(prefix, fence) + "root");

    internal static string KeyPrefix(string prefix, ZLinkAggregateFence fence) =>
        FormattableString.Invariant(
            $"{prefix}aggregate-inventory:{fence.AggregateId:D}:{fence.AggregateGeneration}:"
        );

    internal static ZLinkStoreKey PageKey(
        string prefix,
        ZLinkAggregateFence fence,
        int level,
        int index
    ) => new(KeyPrefix(prefix, fence) + FormattableString.Invariant($"page:{level}:{index}"));

    internal static ZLinkStoreKey ParticipantKey(
        string prefix,
        ZLinkAggregateFence fence,
        int index,
        bool membership
    ) =>
        new(
            KeyPrefix(prefix, fence)
                + FormattableString.Invariant($"participant:{index}:")
                + (membership ? "membershipMutation" : "authorityPayload")
        );

    internal static IReadOnlyList<ZLinkRelocationParticipantEnvelope> BindRestoreOrder(
        IReadOnlyList<Participant> inventory,
        IReadOnlyList<ZLinkRelocationParticipantEnvelope> states
    )
    {
        if (states.Count != inventory.Count)
            throw new ZLinkRelocationDataLostException(
                "SPOT relocation state count differs from its sealed inventory."
            );
        for (var index = 0; index < inventory.Count; index++)
            if (
                states[index].CanonicalParticipantId
                != checked((ulong)inventory[index].Entry.Index + 1)
            )
                throw new ZLinkRelocationDataLostException(
                    "SPOT relocation state order differs from its sealed inventory."
                );
        return states;
    }

    internal static ReadOnlyMemory<byte> MembershipBody(ReadOnlyMemory<byte> mutation) =>
        mutation.IsEmpty ? NoMembershipMutation : mutation;

    internal static Participant[] Prepare(IReadOnlyList<ZLinkAggregateParticipant> participants) =>
        // Canonical authority keys are ASCII, including percent-encoded identities.
        participants
            .OrderBy(static participant => participant.Key.Value, StringComparer.Ordinal)
            .Select(
                static (participant, index) =>
                {
                    var mutation = participant.MembershipMutation;
                    if (participant.AuthorityPayload.Length > MaxPageBytes)
                        throw new ArgumentOutOfRangeException(
                            nameof(participants),
                            "Invalid authority payload size."
                        );
                    if (!ValidMembershipMutationLength(mutation.Length))
                        throw new ArgumentOutOfRangeException(
                            nameof(participants),
                            "Invalid membership mutation size."
                        );
                    var entry = new Entry(
                        participant.Key.Value,
                        Hash(participant.AuthorityPayload.Span),
                        participant.ExpectedStoreVersion,
                        index,
                        Hash(mutation.Span),
                        participant.OwnerTransition == ZLinkAuthorityGenerationTransition.NewOwner
                            ? "newOwner"
                            : "preserve"
                    );
                    if (!IsEntryValid(entry))
                        throw new ArgumentException(
                            "Invalid aggregate inventory participant.",
                            nameof(participants)
                        );
                    return new Participant(entry, participant.AuthorityPayload, mutation);
                }
            )
            .ToArray();

    internal static Tree Plan(IReadOnlyList<ZLinkAggregateParticipant> participants) =>
        Build(Prepare(participants).Select(static participant => participant.Entry).ToArray());

    internal static Tree Build(IReadOnlyList<Entry> entries)
    {
        for (var index = 1; index < entries.Count; index++)
            if (
                StringComparer.Ordinal.Compare(
                    entries[index - 1].AuthorityKey,
                    entries[index].AuthorityKey
                ) >= 0
            )
                throw new ArgumentException(
                    "Aggregate inventory authority keys must be strictly ordered.",
                    nameof(entries)
                );
        var pages = new List<(Page, byte[])>();
        var level = 0;
        var references = Pack(
            entries.Count,
            (index, start, count) =>
                new Page([], entries.Skip(start).Take(count).ToArray(), count, index, 0, start)
        );
        while (references.Length > 1)
        {
            level++;
            var previous = references;
            references = Pack(
                previous.Length,
                (index, start, count) =>
                {
                    var children = previous.AsSpan(start, count).ToArray();
                    return new Page(
                        children,
                        [],
                        children.Sum(static child => child.EntryCount),
                        index,
                        level,
                        children[0].StartIndex
                    );
                }
            );
        }
        return new Tree(
            new Root(
                RootDigest(level, references, entries.Count),
                level,
                references,
                entries.Count
            ),
            pages
        );

        Reference[] Pack(int itemCount, Func<int, int, int, Page> createPage)
        {
            var result = new List<Reference>();
            for (var start = 0; start < itemCount; )
            {
                var count = Math.Min(MaxPageItems, itemCount - start);
                var page = createPage(result.Count, start, count);
                var bytes = Encode(page);
                if (bytes.Length > MaxPageBytes)
                {
                    var lower = 1;
                    var upper = count - 1;
                    while (lower <= upper)
                    {
                        var candidateCount = lower + (upper - lower) / 2;
                        var candidate = createPage(result.Count, start, candidateCount);
                        var encoded = Encode(candidate);
                        if (encoded.Length <= MaxPageBytes)
                        {
                            page = candidate;
                            bytes = encoded;
                            count = candidateCount;
                            lower = candidateCount + 1;
                        }
                        else
                            upper = candidateCount - 1;
                    }
                    if (bytes.Length > MaxPageBytes)
                        throw new InvalidDataException(
                            "An aggregate inventory page item exceeds 1 MiB."
                        );
                }
                pages.Add((page, bytes));
                result.Add(
                    new Reference(
                        page.EntryCount,
                        page.Index,
                        page.Level,
                        Hash(bytes),
                        page.StartIndex
                    )
                );
                start += count;
            }
            return result.ToArray();
        }
    }

    private static string RootDigest(int topLevel, Reference[] topPages, int totalCount) =>
        Hash(
            Encode(
                new
                {
                    topLevel,
                    topPages,
                    totalCount,
                }
            )
        );

    private static IEnumerable<(ZLinkStoreKey Key, ReadOnlyMemory<byte> Bytes)> ChildRecords(
        string prefix,
        ZLinkAggregateFence fence,
        Tree tree,
        IReadOnlyList<Participant> participants
    )
    {
        foreach (var (page, bytes) in tree.Pages)
            yield return (PageKey(prefix, fence, page.Level, page.Index), bytes);
        foreach (var participant in participants)
        {
            yield return (
                ParticipantKey(prefix, fence, participant.Entry.Index, false),
                participant.Payload
            );
            yield return (
                ParticipantKey(prefix, fence, participant.Entry.Index, true),
                participant.Mutation
            );
        }
    }

    internal static async ValueTask<string> SealAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        CancellationToken cancellationToken
    )
    {
        var prepared = Prepare(participants);
        var tree = Build(prepared.Select(static participant => participant.Entry).ToArray());
        await WriteAsync(store, prefix, fence, tree, prepared, cancellationToken)
            .ConfigureAwait(false);
        return tree.Root.DigestSha256;
    }

    internal static async ValueTask WriteAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        Tree tree,
        IReadOnlyList<Participant> participants,
        CancellationToken cancellationToken
    )
    {
        var rootKey = RootKey(prefix, fence);
        var rootBytes = Encode(tree.Root);
        var existingRoot = await store.ReadAsync(rootKey, cancellationToken).ConfigureAwait(false);
        var alreadySealed = existingRoot is ZLinkStoreReadResult.Found;
        if (
            existingRoot is ZLinkStoreReadResult.Found foundRoot
            && !foundRoot.Value.Bytes.Span.SequenceEqual(rootBytes)
        )
            throw DataLost(rootKey);

        await Parallel
            .ForEachAsync(
                ChildRecords(prefix, fence, tree, participants),
                new ParallelOptions
                {
                    CancellationToken = cancellationToken,
                    MaxDegreeOfParallelism = 64,
                },
                async (record, token) =>
                    await PutAsync(record.Key, record.Bytes, token).ConfigureAwait(false)
            )
            .ConfigureAwait(false);
        if (!alreadySealed)
            await PutAsync(rootKey, rootBytes, cancellationToken).ConfigureAwait(false);

        async ValueTask PutAsync(
            ZLinkStoreKey key,
            ReadOnlyMemory<byte> bytes,
            CancellationToken token
        )
        {
            if (!alreadySealed)
            {
                IReadOnlyList<ZLinkStoreCondition> conditions =
                    key == rootKey
                        ? [new ZLinkStoreCondition.Missing(rootKey)]
                        :
                        [
                            new ZLinkStoreCondition.Missing(rootKey),
                            new ZLinkStoreCondition.Missing(key),
                        ];
                var write = await store
                    .WriteAsync(
                        new ZLinkStoreWriteRequest(
                            conditions,
                            [new ZLinkStoreMutation.Put(key, bytes, null)]
                        ),
                        token
                    )
                    .ConfigureAwait(false);
                if (write is ZLinkStoreWriteResult.Applied)
                    return;
            }

            var existing = await ReadBytesAsync(store, key, token).ConfigureAwait(false);
            if (!existing.Span.SequenceEqual(bytes.Span))
                throw DataLost(key);
        }
    }

    internal static async ValueTask RemoveInventoryAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        CancellationToken cancellationToken
    )
    {
        var inventoryPrefix = KeyPrefix(prefix, fence);
        await RemoveAsync(store, inventoryPrefix + "page:", cancellationToken)
            .ConfigureAwait(false);
        await RemoveAsync(store, inventoryPrefix + "participant:", cancellationToken)
            .ConfigureAwait(false);
        var root = RootKey(prefix, fence);
        await store
            .WriteAsync(
                new ZLinkStoreWriteRequest([], [new ZLinkStoreMutation.Delete(root)]),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    internal static async ValueTask RemoveAsync(
        IZLinkLocationStore store,
        string childPrefix,
        CancellationToken cancellationToken
    )
    {
        var keys = new List<ZLinkStoreKey>();
        ZLinkStoreScanCursor? cursor = null;
        do
        {
            var scan = await store
                .ScanAsync(new ZLinkStoreScanRequest(childPrefix, cursor, 1000), cancellationToken)
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                keys.Clear();
                cursor = null;
                continue;
            }
            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            keys.AddRange(page.Items.Select(static item => item.Key));
            cursor = page.NextCursor;
        } while (cursor is not null);

        const int maximumDeletesPerBatch = 1024;
        for (var offset = 0; offset < keys.Count; offset += maximumDeletesPerBatch)
        {
            var mutations = keys.Skip(offset)
                .Take(maximumDeletesPerBatch)
                .Select(static key => (ZLinkStoreMutation)new ZLinkStoreMutation.Delete(key))
                .ToArray();
            await store
                .WriteAsync(new ZLinkStoreWriteRequest([], mutations), cancellationToken)
                .ConfigureAwait(false);
        }
    }

    internal static async ValueTask<Root> ReadRootAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        string expectedDigest,
        CancellationToken cancellationToken
    )
    {
        var root = await ReadJsonAsync<Root>(store, RootKey(prefix, fence), cancellationToken)
            .ConfigureAwait(false);
        if (
            root.TotalCount < 0
            || root.TopLevel is < 0 or >= MaxTreeLevels
            || root.TopPages is null
            || root.TopPages.Length != (root.TotalCount == 0 ? 0 : 1)
            || (root.TotalCount == 0 && root.TopLevel != 0)
            || (
                root.TopPages.Length != 0
                && root.TopPages[0] is not { Index: 0, StartIndex: 0, Sha256.Length: 64 }
            )
            || (
                root.TopPages.Length != 0
                && (
                    !IsHash(root.TopPages[0].Sha256)
                    || root.TopPages[0].Level != root.TopLevel
                    || root.TopPages[0].EntryCount != root.TotalCount
                )
            )
            || !string.Equals(root.DigestSha256, expectedDigest, StringComparison.Ordinal)
            || root.DigestSha256 != RootDigest(root.TopLevel, root.TopPages, root.TotalCount)
        )
            throw DataLost(RootKey(prefix, fence));
        return root;
    }

    internal static async ValueTask<IReadOnlyList<Participant>> ReadAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        string expectedDigest,
        CancellationToken cancellationToken
    )
    {
        var entries = await ReadEntriesAsync(
                store,
                prefix,
                fence,
                expectedDigest,
                cancellationToken
            )
            .ConfigureAwait(false);
        var participants = new Participant[entries.Count];
        await Parallel
            .ForEachAsync(
                entries,
                new ParallelOptions
                {
                    MaxDegreeOfParallelism = 64,
                    CancellationToken = cancellationToken,
                },
                async (entry, token) =>
                    participants[entry.Index] = await ReadParticipantAsync(
                            store,
                            prefix,
                            fence,
                            entry,
                            token
                        )
                        .ConfigureAwait(false)
            )
            .ConfigureAwait(false);
        return participants;
    }

    internal static async ValueTask<IReadOnlyList<Entry>> ReadEntriesAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        string expectedDigest,
        CancellationToken cancellationToken
    )
    {
        var root = await ReadRootAsync(store, prefix, fence, expectedDigest, cancellationToken)
            .ConfigureAwait(false);
        var entries = new List<Entry>();
        foreach (var reference in root.TopPages)
            await ReadEntriesAsync(reference).ConfigureAwait(false);
        if (entries.Count != root.TotalCount || Build(entries).Root.DigestSha256 != expectedDigest)
            throw DataLost(RootKey(prefix, fence));
        return entries;

        async ValueTask ReadEntriesAsync(Reference reference)
        {
            var page = await ReadPageAsync(store, prefix, fence, reference, cancellationToken)
                .ConfigureAwait(false);
            foreach (var entry in page.Entries)
            {
                if (
                    entries.Count != entry.Index
                    || (
                        entries.Count != 0
                        && StringComparer.Ordinal.Compare(
                            entries[^1].AuthorityKey,
                            entry.AuthorityKey
                        ) >= 0
                    )
                )
                    throw DataLost(PageKey(prefix, fence, page.Level, page.Index));
                entries.Add(entry);
            }
            foreach (var child in page.Children)
                await ReadEntriesAsync(child).ConfigureAwait(false);
        }
    }

    internal static async ValueTask<Page> ReadPageAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        Reference reference,
        CancellationToken cancellationToken
    )
    {
        var key = PageKey(prefix, fence, reference.Level, reference.Index);
        var page = await ReadJsonAsync<Page>(store, key, cancellationToken, reference.Sha256)
            .ConfigureAwait(false);
        if (
            page.Level != reference.Level
            || page.Index != reference.Index
            || page.StartIndex != reference.StartIndex
            || page.EntryCount != reference.EntryCount
            || page.EntryCount <= 0
            || page.Entries is null
            || page.Children is null
        )
            throw DataLost(key);
        if (page.Level == 0)
        {
            if (
                page.Children.Length != 0
                || page.Entries.Length != page.EntryCount
                || page.EntryCount > MaxPageItems
            )
                throw DataLost(key);
            for (var offset = 0; offset < page.Entries.Length; offset++)
                if (
                    !IsEntryValid(page.Entries[offset])
                    || page.Entries[offset].Index != (long)page.StartIndex + offset
                )
                    throw DataLost(key);
        }
        else
        {
            if (page.Entries.Length != 0 || page.Children.Length is < 1 or > MaxPageItems)
                throw DataLost(key);
            long next = page.StartIndex;
            foreach (var child in page.Children)
            {
                if (
                    child is null
                    || child.Level != page.Level - 1
                    || child.Index < 0
                    || child.StartIndex != next
                    || child.EntryCount <= 0
                    || !IsHash(child.Sha256)
                )
                    throw DataLost(key);
                next += child.EntryCount;
            }
            if (next != (long)page.StartIndex + page.EntryCount)
                throw DataLost(key);
        }
        return page;
    }

    internal static async ValueTask<Participant> ReadParticipantAsync(
        IZLinkLocationStore store,
        string prefix,
        ZLinkAggregateFence fence,
        Entry entry,
        CancellationToken cancellationToken
    )
    {
        var payloadKey = ParticipantKey(prefix, fence, entry.Index, false);
        var mutationKey = ParticipantKey(prefix, fence, entry.Index, true);
        var payload = await ReadBytesAsync(store, payloadKey, cancellationToken)
            .ConfigureAwait(false);
        var mutation = await ReadBytesAsync(store, mutationKey, cancellationToken)
            .ConfigureAwait(false);
        if (payload.Length > MaxPageBytes || Hash(payload.Span) != entry.AuthorityPayloadSha256)
            throw DataLost(payloadKey);
        if (
            !ValidMembershipMutationLength(mutation.Length)
            || Hash(mutation.Span) != entry.MembershipMutationSha256
        )
            throw DataLost(mutationKey);
        return new Participant(entry, payload, mutation);
    }

    private static bool ValidMembershipMutationLength(int length) =>
        length is >= 1 and <= MaxMembershipMutationBytes;

    private static async ValueTask<T> ReadJsonAsync<T>(
        IZLinkLocationStore store,
        ZLinkStoreKey key,
        CancellationToken cancellationToken,
        string? expectedHash = null
    )
    {
        var bytes = await ReadBytesAsync(store, key, cancellationToken).ConfigureAwait(false);
        if (
            bytes.Length > MaxPageBytes
            || (expectedHash is not null && Hash(bytes.Span) != expectedHash)
        )
            throw DataLost(key);
        try
        {
            return Decode<T>(bytes);
        }
        catch (Exception error)
            when (error
                    is JsonException
                        or ArgumentException
                        or InvalidOperationException
                        or InvalidDataException
            )
        {
            throw DataLost(key);
        }
    }

    internal static T Decode<T>(ReadOnlyMemory<byte> bytes)
    {
        var record = JsonSerializer.Deserialize<T>(bytes.Span, CanonicalJson);
        if (record is null || !bytes.Span.SequenceEqual(Encode(record)))
            throw new InvalidDataException("Aggregate record is not canonical JSON.");
        return record;
    }

    private static async ValueTask<ReadOnlyMemory<byte>> ReadBytesAsync(
        IZLinkLocationStore store,
        ZLinkStoreKey key,
        CancellationToken cancellationToken
    ) =>
        await store.ReadAsync(key, cancellationToken).ConfigureAwait(false)
            is ZLinkStoreReadResult.Found found
            ? found.Value.Bytes
            : throw DataLost(key);

    internal static byte[] Encode<T>(T record) =>
        JsonSerializer.SerializeToUtf8Bytes(record, CanonicalJson);

    internal static string Hash(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static bool IsHash(string? value) =>
        value is { Length: 64 }
        && value.All(static digit => digit is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static bool IsEntryValid(Entry? entry) =>
        entry is not null
        && (
            ZLinkAuthorityKeyCodec.TryDecodeActor(new(entry.AuthorityKey), out _)
            || ZLinkAuthorityKeyCodec.TryDecodeSpot(new(entry.AuthorityKey), out _)
        )
        && !string.IsNullOrEmpty(entry.ExpectedStoreVersion)
        && !entry.ExpectedStoreVersion.Contains('\0')
        && StrictUtf8.GetByteCount(entry.ExpectedStoreVersion) <= 4096
        && entry.OwnerTransition is "preserve" or "newOwner"
        && IsHash(entry.AuthorityPayloadSha256)
        && IsHash(entry.MembershipMutationSha256);

    internal static ZLinkRelocationDataLostException DataLost(ZLinkStoreKey key) =>
        new($"Aggregate inventory record '{key.Value}' is missing or invalid.");

    private sealed class CanonicalStringConverter : JsonConverter<string>
    {
        public override string? Read(
            ref Utf8JsonReader reader,
            Type type,
            JsonSerializerOptions options
        ) => reader.GetString();

        public override void Write(
            Utf8JsonWriter writer,
            string value,
            JsonSerializerOptions options
        )
        {
            var json = new StringBuilder(value.Length + 2).Append('"');
            foreach (var character in value)
            {
                switch (character)
                {
                    case '"':
                        json.Append("\\\"");
                        break;
                    case '\\':
                        json.Append("\\\\");
                        break;
                    case '\b':
                        json.Append("\\b");
                        break;
                    case '\t':
                        json.Append("\\t");
                        break;
                    case '\n':
                        json.Append("\\n");
                        break;
                    case '\f':
                        json.Append("\\f");
                        break;
                    case '\r':
                        json.Append("\\r");
                        break;
                    case < ' ':
                        json.Append("\\u").Append(((ushort)character).ToString("x4"));
                        break;
                    default:
                        json.Append(character);
                        break;
                }
            }
            json.Append('"');
            writer.WriteRawValue(StrictUtf8.GetBytes(json.ToString()));
        }
    }
}
