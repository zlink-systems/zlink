using System.Text.Json;
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
        var descriptorRids = new Dictionary<string, ILookup<string, RoutingId>>(
            StringComparer.Ordinal
        );
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
                var (mesh, display) = DecodeLegacyCapacityKey(row.Key);
                if (!descriptorRids.TryGetValue(mesh, out var rids))
                {
                    rids = await ScanDescriptorRidsAsync(mesh, cancellationToken)
                        .ConfigureAwait(false);
                    descriptorRids.Add(mesh, rids);
                }
                await MoveLegacyCapacityRowAsync(row.Key, mesh, display, rids, cancellationToken)
                    .ConfigureAwait(false);
            }
            // A fresh scan checks completion after the final page of the old snapshot.
            cursor = page.NextCursor;
        }
    }

    private async ValueTask<ILookup<string, RoutingId>> ScanDescriptorRidsAsync(
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
                return rids.ToLookup(rid => rid.ToString(), StringComparer.Ordinal);
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
        string display,
        ILookup<string, RoutingId> descriptorRids,
        CancellationToken cancellationToken
    )
    {
        // A node can publish new descriptors after conversion. Check that the
        // old row still exists after scanning those descriptors.
        var old = await _provider.ReadAsync(oldKey, cancellationToken).ConfigureAwait(false);
        if (old is not ZLinkStoreReadResult.Found)
            return;

        var candidates = descriptorRids[display].Take(2).ToArray();
        if (candidates.Length > 1)
            throw new ZLinkConfigurationException(
                $"Ambiguous RoutingId in v11 capacity key {oldKey.Value}."
            );
        var ridHex = (
            candidates.Length == 1 ? candidates[0] : ParseLegacyRoutingId(display)
        ).ToHex();
        while (old is ZLinkStoreReadResult.Found found)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var counters = DecodeLegacyCapacity(oldKey, found.Value.Bytes, mesh, ridHex);
            var conditions = new List<ZLinkStoreCondition>(counters.Count + 1)
            {
                new ZLinkStoreCondition.Value(oldKey, found.Value.Bytes),
            };
            var mutations = new List<ZLinkStoreMutation>(counters.Count + 1);
            foreach (var counter in counters)
            {
                conditions.Add(new ZLinkStoreCondition.Missing(counter.Key));
                mutations.Add(counter);
            }
            mutations.Add(new ZLinkStoreMutation.Delete(oldKey));
            if (
                await WriteMigrationBatchAsync(new(conditions, mutations), cancellationToken)
                    .ConfigureAwait(false)
            )
                return;

            old = await _provider.ReadAsync(oldKey, cancellationToken).ConfigureAwait(false);
            if (old is ZLinkStoreReadResult.Missing)
            {
                foreach (var counter in counters)
                    if (
                        await _provider
                            .ReadAsync(counter.Key, cancellationToken)
                            .ConfigureAwait(false) is ZLinkStoreReadResult.Missing
                    )
                        throw new ZLinkConfigurationException(
                            $"The migrated Location Store counter {counter.Key.Value} is missing."
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

    private static (string Mesh, string Display) DecodeLegacyCapacityKey(ZLinkStoreKey key)
    {
        var suffix = key.Value[LegacyCapacityPrefix.Length..];
        var separator = suffix.IndexOf(':');
        if (separator <= 0 || separator == suffix.Length - 1)
            throw new ZLinkConfigurationException($"Malformed v11 capacity key {key.Value}.");
        return (
            Uri.UnescapeDataString(suffix[..separator]),
            Uri.UnescapeDataString(suffix[(separator + 1)..])
        );
    }

    private static RoutingId ParseLegacyRoutingId(string display)
    {
        try
        {
            // A display string longer than 255 bytes cannot be a text RID.
            return display.Length > 255 && display.StartsWith("hex:", StringComparison.Ordinal)
                ? RoutingId.FromHex(display[4..])
                : RoutingId.From(display);
        }
        catch (ArgumentException error)
        {
            throw new ZLinkConfigurationException(
                $"Invalid v11 capacity RoutingId. {error.Message}"
            );
        }
    }

    private static List<ZLinkStoreMutation.Put> DecodeLegacyCapacity(
        ZLinkStoreKey oldKey,
        ReadOnlyMemory<byte> bytes,
        string mesh,
        string ridHex
    )
    {
        var counters = new List<ZLinkStoreMutation.Put>();
        try
        {
            using var document = JsonDocument.Parse(bytes);
            ReadUsage(document.RootElement.GetProperty("active"), pending: false);
            ReadUsage(document.RootElement.GetProperty("pending"), pending: true);
        }
        catch (Exception error)
            when (error is JsonException or InvalidOperationException or KeyNotFoundException)
        {
            throw new ZLinkConfigurationException($"Invalid v11 capacity record {oldKey.Value}.");
        }
        if (counters.Count >= ZLinkLocationStoreLimits.MaximumUniqueKeys)
            throw new ZLinkConfigurationException(
                $"Conversion of v11 capacity record {oldKey.Value} exceeds the Location Store batch key limit."
            );
        return counters;

        void ReadUsage(JsonElement usage, bool pending)
        {
            ReadCounter("actors", usage.GetProperty("actors"), pending);
            ReadCounter("spots", usage.GetProperty("spots"), pending);
            foreach (var property in usage.GetProperty("spotTypes").EnumerateObject())
            {
                var separator = property.Name.IndexOf('\0');
                if (separator < 0)
                    throw new ZLinkConfigurationException(
                        $"Invalid spot type in v11 capacity record {oldKey.Value}."
                    );
                var kind = property.Name[..separator] switch
                {
                    "user_spot" => ZLinkPlacementObjectKind.UserSpot,
                    "instance_spot" => ZLinkPlacementObjectKind.InstanceSpot,
                    _ => throw new ZLinkConfigurationException(
                        $"Invalid spot kind in v11 capacity record {oldKey.Value}."
                    ),
                };
                ReadCounter(
                    SpotCapacityDimension(kind, property.Name[(separator + 1)..]),
                    property.Value,
                    pending
                );
            }
        }

        void ReadCounter(string dimension, JsonElement count, bool pending)
        {
            if (!count.TryGetInt64(out var value) || value < 0)
                throw new ZLinkConfigurationException(
                    $"Invalid count in v11 capacity record {oldKey.Value}."
                );
            if (value != 0)
                counters.Add(
                    new(
                        CapacityCounterKey(mesh, ridHex, dimension, pending),
                        EncodeGenerationCounter((ulong)value),
                        null
                    )
                );
        }
    }
}
