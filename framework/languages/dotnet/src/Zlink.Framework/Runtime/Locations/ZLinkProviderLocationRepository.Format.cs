using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Zlink.Framework.Internal;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string LegacyCapacityPrefix = Prefix + "capacity:";
    private static readonly ZLinkStoreKey FormatKey = Key("zlink:v12:format");
    private static readonly ZLinkStoreKey GenerationFloorKey = Key("zlink:v12:generation-floor");
    private static readonly byte[] ConvertedFormat = "converted"u8.ToArray();
    private ulong _generationFloor;

    public async ValueTask EnsureStoreFormatAsync(CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _generationFloor) != 0)
            return;

        var format = await _provider.ReadAsync(FormatKey, cancellationToken).ConfigureAwait(false);
        ulong floor;
        ReadOnlyMemory<byte> marker;
        if (format is ZLinkStoreReadResult.Found found)
        {
            marker = found.Value.Bytes;
            var storedFloor = await _provider
                .ReadAsync(GenerationFloorKey, cancellationToken)
                .ConfigureAwait(false);
            if (storedFloor is not ZLinkStoreReadResult.Found)
                throw new ZLinkConfigurationException(
                    "The converted Location Store has no generation floor."
                );
            floor = DecodeMigrationGeneration(storedFloor);
        }
        else
        {
            await SettleLegacyAggregatesAsync(cancellationToken).ConfigureAwait(false);
            floor = await EnsureGenerationFloorAsync(cancellationToken).ConfigureAwait(false);
            await MoveLegacyCapacityAsync(cancellationToken).ConfigureAwait(false);
            marker = await PutMissingMigrationKeyAsync(
                    FormatKey,
                    ConvertedFormat,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        if (!marker.Span.SequenceEqual(ConvertedFormat))
            throw new ZLinkConfigurationException("The Location Store format is unrecognized.");
        Volatile.Write(ref _generationFloor, floor);
    }

    private async ValueTask<ulong> EnsureGenerationFloorAsync(CancellationToken cancellationToken)
    {
        var current = await _provider
            .ReadAsync(GenerationFloorKey, cancellationToken)
            .ConfigureAwait(false);
        if (current is ZLinkStoreReadResult.Found)
            return DecodeMigrationGeneration(current);

        var legacyObject = await _provider
            .ReadAsync(Key(Prefix + "object-counter"), cancellationToken)
            .ConfigureAwait(false);
        var legacyOwner = await _provider
            .ReadAsync(Key(Prefix + "authority-owner-counter"), cancellationToken)
            .ConfigureAwait(false);
        var floor = Math.Max(
            DecodeMigrationGeneration(legacyObject),
            DecodeMigrationGeneration(legacyOwner)
        );
        var stored = await PutMissingMigrationKeyAsync(
                GenerationFloorKey,
                EncodeGenerationCounter(floor),
                cancellationToken
            )
            .ConfigureAwait(false);
        return DecodeMigrationGeneration(stored);
    }

    private async ValueTask<ReadOnlyMemory<byte>> PutMissingMigrationKeyAsync(
        ZLinkStoreKey key,
        ReadOnlyMemory<byte> value,
        CancellationToken cancellationToken
    )
    {
        var request = new ZLinkStoreWriteRequest(
            [new ZLinkStoreCondition.Missing(key)],
            [new ZLinkStoreMutation.Put(key, value, null)]
        );
        while (!await WriteMigrationBatchAsync(request, cancellationToken).ConfigureAwait(false))
        {
            var current = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
            if (current is ZLinkStoreReadResult.Found found)
                return found.Value.Bytes;
        }
        return value;
    }

    private static ulong DecodeMigrationGeneration(ZLinkStoreReadResult result) =>
        result is ZLinkStoreReadResult.Found found
            ? DecodeMigrationGeneration(found.Value.Bytes)
            : 1;

    private static ulong DecodeMigrationGeneration(ReadOnlyMemory<byte> bytes)
    {
        try
        {
            return DecodeGenerationCounter(bytes);
        }
        catch (InvalidDataException error)
        {
            throw new ZLinkConfigurationException(error.Message);
        }
    }

    private async ValueTask MoveLegacyCapacityAsync(CancellationToken cancellationToken)
    {
        var descriptorRids = new Dictionary<string, HashSet<RoutingId>>(StringComparer.Ordinal);
        ZLinkStoreScanCursor? cursor = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var scan = await ScanMigrationPageAsync(LegacyCapacityPrefix, cursor, cancellationToken)
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                cursor = null;
                continue;
            }

            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            if (cursor is null && page.Items.Count == 0 && page.NextCursor is null)
                return;
            foreach (var row in page.Items)
            {
                var (mesh, legacyRid) = DecodeLegacyCapacityKey(row.Key);
                if (!descriptorRids.TryGetValue(mesh, out var rids))
                {
                    rids = await ScanDescriptorRidsAsync(mesh, cancellationToken)
                        .ConfigureAwait(false);
                    descriptorRids.Add(mesh, rids);
                }
                await MoveLegacyCapacityRowAsync(row.Key, mesh, legacyRid, rids, cancellationToken)
                    .ConfigureAwait(false);
            }
            // A fresh scan checks completion after the final page of the old snapshot.
            cursor = page.NextCursor;
        }
    }

    private async ValueTask<HashSet<RoutingId>> ScanDescriptorRidsAsync(
        string mesh,
        CancellationToken cancellationToken
    )
    {
        var rids = new HashSet<RoutingId>();
        ZLinkStoreScanCursor? cursor = null;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var scan = await ScanMigrationPageAsync(MeshPrefix(mesh), cursor, cancellationToken)
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                rids.Clear();
                cursor = null;
                continue;
            }

            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            foreach (var row in page.Items)
                rids.Add(DecodeDescriptor<ZLinkMeshNodeDescriptor>(row.Value.Bytes).Descriptor.Rid);
            cursor = page.NextCursor;
            if (cursor is null)
                return rids;
        }
    }

    private ValueTask<ZLinkStoreScanResult> ScanMigrationPageAsync(
        string prefix,
        ZLinkStoreScanCursor? cursor,
        CancellationToken cancellationToken
    ) =>
        _provider.ScanAsync(
            new ZLinkStoreScanRequest(prefix, cursor, ZLinkPageRequestPolicy.MaximumPageSize),
            cancellationToken
        );

    private async ValueTask MoveLegacyCapacityRowAsync(
        ZLinkStoreKey oldKey,
        string mesh,
        byte[] legacyRid,
        HashSet<RoutingId> descriptorRids,
        CancellationToken cancellationToken
    )
    {
        // A node can publish new descriptors after conversion. Check that the
        // old row still exists after scanning those descriptors.
        var old = await _provider.ReadAsync(oldKey, cancellationToken).ConfigureAwait(false);
        if (old is not ZLinkStoreReadResult.Found)
            return;

        var candidates = descriptorRids
            .Where(rid =>
                rid.ToBytes().SequenceEqual(legacyRid)
                || Encoding.UTF8.GetBytes(rid.ToString()).AsSpan().SequenceEqual(legacyRid)
            )
            .Take(2)
            .ToArray();
        if (candidates.Length > 1)
            throw new ZLinkConfigurationException(
                $"Ambiguous RoutingId in v11 capacity key {oldKey.Value}."
            );
        string ridHex;
        try
        {
            ridHex = (candidates.Length == 1 ? candidates[0] : RoutingId.From(legacyRid)).ToHex();
        }
        catch (ArgumentException error)
        {
            throw new ZLinkConfigurationException(
                $"Invalid RoutingId in v11 capacity key {oldKey.Value}. {error.Message}"
            );
        }
        while (old is ZLinkStoreReadResult.Found found)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var counters = DecodeLegacyCapacity(oldKey, found.Value.Bytes)
                .Counters.Select(counter => (
                    Key: CapacityCounterKey(mesh, ridHex, counter.Key.Dimension, counter.Key.Pending),
                    Count: counter.Value.Count
                ))
                .ToArray();
            if (counters.Length >= ZLinkLocationStoreLimits.MaximumUniqueKeys)
                throw new ZLinkConfigurationException(
                    $"Conversion of v11 capacity record {oldKey.Value} exceeds the Location Store batch key limit."
                );
            var conditions = new List<ZLinkStoreCondition>(counters.Length + 1)
            {
                new ZLinkStoreCondition.Value(oldKey, found.Value.Bytes),
            };
            var mutations = new List<ZLinkStoreMutation>(counters.Length + 1);
            foreach (var counter in counters)
            {
                conditions.Add(new ZLinkStoreCondition.Missing(counter.Key));
                if (counter.Count != 0)
                    mutations.Add(new ZLinkStoreMutation.Put(counter.Key, EncodeGenerationCounter((ulong)counter.Count), null));
            }
            mutations.Add(new ZLinkStoreMutation.Delete(oldKey));
            if (
                await WriteMigrationBatchAsync(new(conditions, mutations), cancellationToken)
                    .ConfigureAwait(false)
            )
                return;

            old = await _provider.ReadAsync(oldKey, cancellationToken).ConfigureAwait(false);
            if (old is ZLinkStoreReadResult.Found current && current.Value.Bytes.Span.SequenceEqual(found.Value.Bytes.Span))
            {
                foreach (var counter in counters)
                    if (
                        await _provider
                            .ReadAsync(counter.Key, cancellationToken)
                            .ConfigureAwait(false) is ZLinkStoreReadResult.Found
                    )
                        throw new ZLinkConfigurationException(
                            $"The Location Store counter {counter.Key.Value} already exists during conversion."
                        );
            }
        }
    }

    private async ValueTask<bool> WriteMigrationBatchAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            var result = await _provider
                .WriteAsync(request, cancellationToken)
                .ConfigureAwait(false);
            return result switch
            {
                ZLinkStoreWriteResult.Applied => true,
                ZLinkStoreWriteResult.Conflict => false,
                _ => throw new ZLinkConfigurationException(
                    "The Location Store rejected a conversion batch."
                ),
            };
        }
        catch (ArgumentException error)
        {
            throw new ZLinkConfigurationException(
                $"Invalid Location Store conversion batch. {error.Message}"
            );
        }
        catch (ZLinkFrameworkException error)
            when (error.Kind == ZLinkFrameworkErrorKind.Unavailable)
        {
            // A write may have committed before its response was lost. The caller reads its
            // conditions and mutations before deciding whether another write is needed.
            return false;
        }
    }

    private static (string Mesh, byte[] Rid) DecodeLegacyCapacityKey(ZLinkStoreKey key)
    {
        var suffix = key.Value[LegacyCapacityPrefix.Length..];
        var separator = suffix.IndexOf(':');
        if (separator <= 0 || separator == suffix.Length - 1)
            throw new ZLinkConfigurationException($"Malformed v11 capacity key {key.Value}.");
        var rid = Encoding.UTF8.GetBytes(suffix[(separator + 1)..]);
        var length = 0;
        for (var index = 0; index < rid.Length; index++)
        {
            var next = rid[index];
            if (
                next == '%'
                && index + 2 < rid.Length
                && Uri.IsHexDigit((char)rid[index + 1])
                && Uri.IsHexDigit((char)rid[index + 2])
            )
            {
                next = (byte)(
                    (Uri.FromHex((char)rid[index + 1]) << 4) | Uri.FromHex((char)rid[index + 2])
                );
                index += 2;
            }
            rid[length++] = next;
        }
        return (Uri.UnescapeDataString(suffix[..separator]), rid[..length]);
    }

    private sealed record LegacyCapacityField(JsonObject Row, string Name, long Count);

    private static (
        JsonObject Record,
        Dictionary<(string Dimension, bool Pending), LegacyCapacityField> Counters
    ) DecodeLegacyCapacity(ZLinkStoreKey key, ReadOnlyMemory<byte> bytes)
    {
        var counters = new Dictionary<(string, bool), LegacyCapacityField>();
        try
        {
            var root = JsonNode.Parse(bytes.Span)?.AsObject() ?? throw new JsonException();
            if (root.ContainsKey("active") || root.ContainsKey("pending"))
            {
                ReadUsage(RequireObject(root, "active"), pending: false);
                ReadUsage(RequireObject(root, "pending"), pending: true);
            }
            else
            {
                ReadCounter(root, "actorsActive", "actors", pending: false);
                ReadCounter(root, "actorsPending", "actors", pending: true);
                ReadCounter(root, "spotsActive", "spots", pending: false);
                ReadCounter(root, "spotsPending", "spots", pending: true);
                foreach (var (name, value) in RequireObject(root, "spotTypes"))
                {
                    var dimension = DecodeType(name, ':');
                    var counts = value?.AsObject() ?? throw new JsonException();
                    ReadCounter(counts, "active", dimension, pending: false);
                    ReadCounter(counts, "pending", dimension, pending: true);
                }
            }
            return (root, counters);
        }
        catch (Exception error)
            when (error
                    is JsonException
                        or InvalidOperationException
                        or ArgumentException
                        or FormatException
            )
        {
            throw new ZLinkConfigurationException($"Invalid v11 capacity record {key.Value}.");
        }

        void ReadUsage(JsonObject usage, bool pending)
        {
            ReadCounter(usage, "actors", "actors", pending);
            ReadCounter(usage, "spots", "spots", pending);
            var types = RequireObject(usage, "spotTypes");
            foreach (var (name, _) in types)
                ReadCounter(types, name, DecodeType(name, '\0'), pending);
        }

        void ReadCounter(JsonObject row, string name, string dimension, bool pending)
        {
            if (
                row[name] is not JsonValue count
                || !count.TryGetValue<long>(out var value)
                || value < 0
            )
                throw new JsonException();
            counters.Add((dimension, pending), new(row, name, value));
        }

        static JsonObject RequireObject(JsonObject row, string name) =>
            row[name]?.AsObject() ?? throw new JsonException();

        static string DecodeType(string name, char delimiter)
        {
            var separator = name.IndexOf(delimiter);
            if (separator < 0)
                throw new JsonException();
            var kind = (delimiter, name[..separator]) switch
            {
                (':', "2") or ('\0', "user_spot") => ZLinkPlacementObjectKind.UserSpot,
                (':', "3") or ('\0', "instance_spot") => ZLinkPlacementObjectKind.InstanceSpot,
                _ => throw new JsonException(),
            };
            return SpotCapacityDimension(kind, name[(separator + 1)..]);
        }
    }

    private static string EncodeLegacyCapacityComponent(string value) =>
        Uri.EscapeDataString(value)
            .Replace("%21", "!", StringComparison.Ordinal)
            .Replace("%27", "'", StringComparison.Ordinal)
            .Replace("%28", "(", StringComparison.Ordinal)
            .Replace("%29", ")", StringComparison.Ordinal)
            .Replace("%2A", "*", StringComparison.Ordinal);

    private async ValueTask AppendLegacyCapacityReleaseAsync(
        AggregateHeader header,
        ICollection<ZLinkStoreCondition> conditions,
        ICollection<ZLinkStoreMutation> mutations,
        CancellationToken cancellationToken
    )
    {
        var dimensions = CapacityDimensions(header.Capacity).ToArray();
        if (dimensions.Length == 0)
            return;

        // v11 .NET wrote capacity rows using the descriptor's display RID.
        var target = header.TargetDescriptor;
        var key = Key(
            $"{LegacyCapacityPrefix}{EncodeLegacyCapacityComponent(target.MeshName)}:{EncodeLegacyCapacityComponent(target.Rid.ToString())}"
        );
        var stored = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
        if (stored is not ZLinkStoreReadResult.Found found)
            throw new ZLinkConfigurationException(
                $"Missing v11 aggregate capacity row {key.Value}."
            );
        var (record, counters) = DecodeLegacyCapacity(key, found.Value.Bytes);
        foreach (var (dimension, count) in dimensions)
        {
            if (!counters.TryGetValue((dimension, true), out var field) || field.Count < count)
                throw new ZLinkConfigurationException(
                    $"Invalid pending v11 aggregate capacity in {key.Value}."
                );
            field.Row[field.Name] = field.Count - count;
        }
        conditions.Add(new ZLinkStoreCondition.Value(key, found.Value.Bytes));
        mutations.Add(new ZLinkStoreMutation.Put(key, Encode(record), null));
    }

    private async ValueTask<bool> CanFinishLegacyWithoutParticipantsAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken
    )
    {
        if (aggregate.ParticipantsCleaned)
            return true;
        if (
            aggregate.Status == AggregateStatus.Prepared
            || (aggregate.Status == AggregateStatus.Committed && !aggregate.ParticipantsNormalized)
        )
            return false;
        var prefix = AggregateKey(fence).Value + ":participant:";
        var scan = await _provider
            .ScanAsync(new(prefix, null, 1), cancellationToken)
            .ConfigureAwait(false);
        return scan is ZLinkStoreScanResult.Page { Value: { Items.Count: 0, NextCursor: null } };
    }

    private async ValueTask<bool> CanFinishLegacyTerminalAsync(
        ZLinkAggregateFence fence,
        AggregateRecord aggregate,
        CancellationToken cancellationToken
    ) =>
        aggregate.Status is AggregateStatus.Aborted or AggregateStatus.Committed
        && await CanFinishLegacyWithoutParticipantsAsync(fence, aggregate, cancellationToken)
            .ConfigureAwait(false);

    private async ValueTask FinishLegacyWithoutParticipantsAsync(
        ZLinkAggregateFence fence,
        StoredRecord<AggregateRecord> stored,
        CancellationToken cancellationToken
    )
    {
        if (stored.Record.ParticipantsCleaned)
            return;
        // Participant rows precede fence installation and are deleted after
        // fence release or normalization. The remaining children are inventory.
        await CleanupUnfencedAggregateChildrenAsync(fence, cancellationToken).ConfigureAwait(false);
        var key = AggregateKey(fence);
        var terminal = stored.Record with
        {
            Status =
                stored.Record.Status == AggregateStatus.Staging
                    ? AggregateStatus.Aborted
                    : stored.Record.Status,
            ParticipantsCleaned = true,
        };
        await _provider
            .WriteAsync(
                new(
                    [new ZLinkStoreCondition.Version(key, stored.Version)],
                    [new ZLinkStoreMutation.Put(key, Encode(terminal), null)]
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private async ValueTask SettleLegacyAggregatesAsync(CancellationToken cancellationToken)
    {
        try
        {
            await RecoverAggregatesAsync(beforeConversion: true, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZLinkRelocationDataLostException error)
        {
            throw new ZLinkConfigurationException(error.Message);
        }
        catch (ZLinkFrameworkException error) when (error.Kind == ZLinkFrameworkErrorKind.DataLost)
        {
            throw new ZLinkConfigurationException(error.Message);
        }
    }
}
