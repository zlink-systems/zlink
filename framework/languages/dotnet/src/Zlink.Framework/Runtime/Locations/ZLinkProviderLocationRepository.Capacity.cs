using System.Globalization;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string CapacityCounterPrefix = "zlink:v12:capacity\0";

    private static string CapacityScope(ZLinkMeshNodeDescriptorKey descriptor) =>
        CapacityScope(descriptor.MeshName, descriptor.Rid.ToHex());

    private static string CapacityScope(string mesh, string routingIdHex) =>
        $"{CapacityCounterPrefix}{mesh}\0{routingIdHex}\0";

    private static ZLinkStoreKey CapacityCounterKey(
        string mesh,
        string routingIdHex,
        string dimension,
        bool pending
    ) => CapacityCounterKey(CapacityScope(mesh, routingIdHex), dimension, pending);

    private static ZLinkStoreKey CapacityCounterKey(string scope, string dimension, bool pending) =>
        Key($"{scope}{dimension}\0{(pending ? "pending" : "active")}");

    private static string SpotCapacityDimension(ZLinkPlacementObjectKind kind, string stableType) =>
        $"spot-type\0{kind switch
        {
            ZLinkPlacementObjectKind.UserSpot => "userSpot",
            ZLinkPlacementObjectKind.InstanceSpot => "instanceSpot",
            _ => throw new ArgumentOutOfRangeException(nameof(kind)),
        }}\0{stableType}";

    private static IEnumerable<(string Dimension, int Count)> CapacityDimensions(
        ZLinkCapacityVector vector
    )
    {
        if (vector.Actors != 0)
            yield return ("actors", vector.Actors);
        if (vector.Spots != 0)
            yield return ("spots", vector.Spots);
        if (vector.SpotType is { } type)
            yield return (SpotCapacityDimension(type.ObjectKind, type.StableType), type.Count);
    }

    private static List<ZLinkStoreCondition>? CapacityReservationConditions(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkCapacityVector vector
    )
    {
        if (vector.Spots != (vector.SpotType?.Count ?? 0))
            return null;
        var typeLimit = 0;
        if (vector.SpotType is { } type)
        {
            var capability = descriptor.ObjectCapabilities.SingleOrDefault(capability =>
                capability.ObjectKind == type.ObjectKind && capability.StableType == type.StableType
            );
            if (capability is null)
                return null;
            typeLimit = capability.Limit;
        }
        var scope = CapacityScope(new(descriptor.MeshName, descriptor.Rid));
        var conditions = new List<ZLinkStoreCondition>();
        foreach (var (dimension, count) in CapacityDimensions(vector))
        {
            var limit = dimension switch
            {
                "actors" => descriptor.Capacity.Actors.Limit,
                "spots" => descriptor.Capacity.Spots.Limit,
                _ => typeLimit,
            };
            if (limit > 0)
                conditions.Add(
                    new ZLinkStoreCondition.SumAtMost(
                        [
                            CapacityCounterKey(scope, dimension, false),
                            CapacityCounterKey(scope, dimension, true),
                        ],
                        count,
                        limit
                    )
                );
        }
        return conditions;
    }

    private async ValueTask<bool> ReservationAuthorityMatchesAsync(
        ZLinkStoreCondition authority,
        StoredTarget target,
        CancellationToken cancellationToken
    ) =>
        await MatchesAsync(authority, cancellationToken).ConfigureAwait(false)
        && await MatchesAsync(target.DescriptorCondition, cancellationToken).ConfigureAwait(false)
        && await MatchesAsync(target.OwnerCondition, cancellationToken).ConfigureAwait(false);

    private async ValueTask<bool> MatchesAsync(
        ZLinkStoreCondition condition,
        CancellationToken cancellationToken
    )
    {
        switch (condition)
        {
            case ZLinkStoreCondition.Missing missing:
                return await _provider
                        .ReadAsync(missing.Key, cancellationToken)
                        .ConfigureAwait(false) is ZLinkStoreReadResult.Missing;
            case ZLinkStoreCondition.Version version:
                return await _provider
                        .ReadAsync(version.Key, cancellationToken)
                        .ConfigureAwait(false)
                        is ZLinkStoreReadResult.Found current
                    && current.Value.Version == version.Expected;
            case ZLinkStoreCondition.Value value:
                return await _provider.ReadAsync(value.Key, cancellationToken).ConfigureAwait(false)
                        is ZLinkStoreReadResult.Found found
                    && found.Value.Bytes.Span.SequenceEqual(value.Expected.Span);
            default:
                throw new ArgumentOutOfRangeException(nameof(condition));
        }
    }

    private async ValueTask<bool> IsReservationCapacityExceededAsync(
        ZLinkMeshNodeDescriptor descriptor,
        IReadOnlyList<ZLinkStoreCondition> conditions,
        CancellationToken cancellationToken
    )
    {
        var result = await _provider
            .WriteAsync(new(conditions, []), cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkStoreWriteResult.Conflict)
            return false;
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"eligible_target_rejected reason=capacity_exceeded mesh={descriptor.MeshName} rid={descriptor.Rid}"
        );
        return true;
    }

    private static void AddCapacityMutations(
        ICollection<ZLinkStoreMutation> mutations,
        ZLinkPlacementAllocation allocation,
        int pending = 0,
        int active = 0
    ) =>
        AddCapacityMutations(
            mutations,
            allocation.Descriptor,
            allocation.Capacity,
            pending,
            active
        );

    private static void AddCapacityMutations(
        ICollection<ZLinkStoreMutation> mutations,
        ZLinkMeshNodeDescriptorKey descriptor,
        ZLinkCapacityVector vector,
        int pending = 0,
        int active = 0
    )
    {
        var deltas = new Dictionary<ZLinkStoreKey, long>();
        AccumulateCapacity(deltas, descriptor, vector, pending, active);
        AppendCapacityMutations(mutations, deltas);
    }

    private static void AccumulateCapacity(
        Dictionary<ZLinkStoreKey, long> deltas,
        ZLinkMeshNodeDescriptorKey descriptor,
        ZLinkCapacityVector vector,
        int pending = 0,
        int active = 0
    )
    {
        var scope = CapacityScope(descriptor);
        foreach (var (dimension, count) in CapacityDimensions(vector))
        {
            if (pending != 0)
                Accumulate(CapacityCounterKey(scope, dimension, true), (long)pending * count);
            if (active != 0)
                Accumulate(CapacityCounterKey(scope, dimension, false), (long)active * count);
        }

        void Accumulate(ZLinkStoreKey key, long delta) =>
            deltas[key] = checked(deltas.GetValueOrDefault(key) + delta);
    }

    private static void AppendCapacityMutations(
        ICollection<ZLinkStoreMutation> mutations,
        Dictionary<ZLinkStoreKey, long> deltas
    )
    {
        foreach (var (key, delta) in deltas)
            if (delta != 0)
                mutations.Add(new ZLinkStoreMutation.Add(key, delta));
    }

    private static int DecodeCapacityCounter(ReadOnlyMemory<byte> bytes)
    {
        // Capacity projections use Int32. Validate the Store's Int64 encoding before projecting.
        var text = Encoding.UTF8.GetString(bytes.Span);
        if (
            !long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var value)
            || text != value.ToString(CultureInfo.InvariantCulture)
        )
            throw new InvalidDataException(
                "The Location Store capacity counter is not canonical decimal."
            );
        return checked((int)value);
    }
}
