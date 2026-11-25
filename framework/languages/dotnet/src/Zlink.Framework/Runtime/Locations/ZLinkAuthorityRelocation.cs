using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkAuthorityRelocation(
    Guid RelocationId,
    [property: JsonConverter(
        typeof(ZLinkCamelCaseEnumJsonConverter<ServiceWireCodec.RelocationPhase>)
    )]
        ServiceWireCodec.RelocationPhase Phase,
    ZLinkAuthorityRelocation.Fence Source,
    ZLinkAuthorityRelocation.TargetFence? Target,
    ZLinkAuthorityRelocation.CoordinatorFence Coordinator,
    string SourceInventorySha256,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
        ulong ApplicationVersion,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        ZLinkRelocationCompletionProgress? Completion = null
)
{
    internal record Fence(
        string NodeRid,
        [property: JsonConverter(
            typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong NodeGeneration,
        string OwnerId,
        [property: JsonConverter(
            typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong OwnerLeaseGeneration
    )
    {
        internal Fence Identity => new(NodeRid, NodeGeneration, OwnerId, OwnerLeaseGeneration);
    }

    internal sealed record TargetFence(
        string NodeRid,
        ulong NodeGeneration,
        string OwnerId,
        ulong OwnerLeaseGeneration,
        [property: JsonPropertyOrder(4)]
        [property: JsonConverter(
            typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong AttemptGeneration
    ) : Fence(NodeRid, NodeGeneration, OwnerId, OwnerLeaseGeneration);

    internal sealed record CoordinatorFence(
        string NodeRid,
        ulong NodeGeneration,
        string OwnerId,
        ulong OwnerLeaseGeneration,
        [property: JsonPropertyOrder(4)] string ExpectedStoreVersion
    ) : Fence(NodeRid, NodeGeneration, OwnerId, OwnerLeaseGeneration);

    internal ZLinkAuthorityRelocation WithAcceptedRequests(ulong? acceptedRequests) =>
        acceptedRequests is { } count
            ? this with
            {
                Completion = ZLinkRelocationCompletionProgress.Start(count),
            }
            : this;

    internal static bool MatchesCommittedTarget(
        ZLinkAuthoritySnapshot authority,
        Guid relocationId,
        ulong attemptGeneration
    ) =>
        authority.Relocation
            is {
                Phase: >= ServiceWireCodec.RelocationPhase.Committed
                    and <= ServiceWireCodec.RelocationPhase.Completed,
                Target: { } target,
            } relocation
        && relocation.RelocationId == relocationId
        && target.AttemptGeneration == attemptGeneration
        && target.NodeRid == authority.Allocation.Descriptor.Rid.ToHex()
        && target.NodeGeneration == authority.Allocation.DescriptorLifecycleGeneration
        && target.OwnerId == authority.OwnerId
        && authority.OwnerLeaseGeneration > 0
        && target.OwnerLeaseGeneration == (ulong)authority.OwnerLeaseGeneration;

    internal bool HoldsCapturedSource(
        Guid relocationId,
        ZLinkAuthoritySnapshot authority,
        Fence coordinator,
        string originalStoreVersion
    ) =>
        RelocationId == relocationId
        && Phase == ServiceWireCodec.RelocationPhase.Captured
        && Source.NodeRid == authority.Allocation.Descriptor.Rid.ToHex()
        && Source.NodeGeneration == authority.Allocation.DescriptorLifecycleGeneration
        && Source.OwnerId == authority.OwnerId
        && authority.OwnerLeaseGeneration > 0
        && Source.OwnerLeaseGeneration == (ulong)authority.OwnerLeaseGeneration
        && Coordinator.NodeRid == coordinator.NodeRid
        && Coordinator.NodeGeneration == coordinator.NodeGeneration
        && Coordinator.OwnerId == coordinator.OwnerId
        && Coordinator.OwnerLeaseGeneration == coordinator.OwnerLeaseGeneration
        && Coordinator.ExpectedStoreVersion == originalStoreVersion;

    internal static ZLinkAuthorityRelocation FromState(
        Guid id,
        ZLinkCanonicalRelocationAuthorityState state,
        string inventorySha256,
        ZLinkRelocationCompletionProgress? completion = null
    ) =>
        new(
            id,
            (ServiceWireCodec.RelocationPhase)state.Phase,
            new(
                state.SourceNodeRid,
                state.SourceNodeGeneration,
                state.SourceOwnerId,
                state.SourceOwnerLeaseGeneration
            ),
            state.TargetAttemptGeneration == 0
                ? null
                : new(
                    state.TargetNodeRid,
                    state.TargetNodeGeneration,
                    state.TargetOwnerId,
                    state.TargetOwnerLeaseGeneration,
                    state.TargetAttemptGeneration
                ),
            new(
                state.CoordinatorNodeRid,
                state.CoordinatorNodeGeneration,
                state.CoordinatorOwnerId,
                state.CoordinatorLeaseGeneration,
                state.CoordinatorExpectedAuthorityStoreVersion
            ),
            inventorySha256,
            checked((ulong)state.ApplicationVersion),
            completion
        );

    internal ZLinkCanonicalRelocationAuthorityProjection Project(
        ReadOnlyMemory<byte> payload,
        ulong aggregateGeneration
    )
    {
        var (high, low) = ZLinkStandaloneActorRelocationPrecommitCoordinator.RelocationParts(
            RelocationId
        );
        var state = new ZLinkCanonicalRelocationAuthorityState(
            high,
            low,
            Target?.AttemptGeneration ?? 0,
            Source.NodeRid,
            Source.NodeGeneration,
            Source.OwnerId,
            Source.OwnerLeaseGeneration,
            Target?.NodeRid ?? "",
            Target?.NodeGeneration ?? 0,
            Target?.OwnerId ?? "",
            Target?.OwnerLeaseGeneration ?? 0,
            Coordinator.OwnerId,
            Coordinator.OwnerLeaseGeneration,
            Coordinator.NodeRid,
            Coordinator.NodeGeneration,
            (byte)Phase,
            unchecked((long)ApplicationVersion)
        )
        {
            AggregateGeneration = aggregateGeneration,
            CoordinatorExpectedAuthorityStoreVersion = Coordinator.ExpectedStoreVersion,
            RelocationReference =
                ZLinkStandaloneActorRelocationPrecommitCoordinator.DirectTransferReference,
        };
        return new(
            high,
            low,
            state.TargetAttemptGeneration,
            state.TargetOwnerId,
            state.TargetOwnerLeaseGeneration,
            state.Phase,
            state.ApplicationVersion,
            state.RelocationReference,
            0,
            payload,
            state
        )
        {
            AggregateGeneration = aggregateGeneration,
        };
    }
}

internal static class ZLinkAuthorityAggregateCodec
{
    private static readonly string[] MarkerFields =
    [
        "aggregateId",
        "aggregateGeneration",
        "index",
        "expectedStoreVersion",
        "ownerTransition",
        "targetAuthorityOwnerGeneration",
        "authorityPayloadSha256",
        "membershipMutationSha256",
    ];

    internal static ZLinkAuthorityRelocation? Read(JsonElement aggregate)
    {
        if (aggregate.ValueKind != JsonValueKind.Object)
            throw InvalidRecord();
        var count = MarkerFields.Count(name => aggregate.TryGetProperty(name, out _));
        var hasRelocation = aggregate.TryGetProperty("relocation", out var value);
        if ((count != 0 && count != MarkerFields.Length) || (count == 0 && !hasRelocation))
            throw InvalidRecord();
        if (!hasRelocation)
            return null;
        var id = Text(value, "relocationId");
        if (!Guid.TryParseExact(id, "D", out var relocationId) || relocationId.ToString("D") != id)
            throw InvalidRecord();
        var phaseText = Text(value, "phase");
        if (
            !Enum.TryParse<ServiceWireCodec.RelocationPhase>(phaseText, true, out var phase)
            || phase
                is < ServiceWireCodec.RelocationPhase.Preparing
                    or > ServiceWireCodec.RelocationPhase.Aborted
            || JsonNamingPolicy.CamelCase.ConvertName(phase.ToString()) != phaseText
        )
            throw InvalidRecord();
        if (
            !value.TryGetProperty("source", out var sourceValue)
            || !value.TryGetProperty("target", out var targetValue)
            || !value.TryGetProperty("coordinator", out var coordinatorValue)
        )
            throw InvalidRecord();
        var source = ReadFence(sourceValue);
        var coordinator = ReadFence(coordinatorValue);
        var target = targetValue.ValueKind == JsonValueKind.Null ? null : ReadFence(targetValue);
        if (
            (
                phase
                    is ServiceWireCodec.RelocationPhase.Preparing
                        or ServiceWireCodec.RelocationPhase.Captured
                && target is not null
            )
            || (
                phase
                    is >= ServiceWireCodec.RelocationPhase.Prepared
                        and <= ServiceWireCodec.RelocationPhase.Completed
                && target is null
            )
        )
            throw InvalidRecord();
        ZLinkRelocationCompletionProgress? completion = null;
        if (value.TryGetProperty("completion", out var progress))
        {
            var accepted = Number(progress, "acceptedRequests");
            var completed = Number(progress, "completedResults");
            var pending = Number(progress, "pendingDeliveries");
            if (
                pending > completed
                || completed > accepted
                || !progress.TryGetProperty("reference", out var reference)
                || !progress.TryGetProperty("sha256", out var sha)
                || (
                    completed == 0
                        ? reference.ValueKind != JsonValueKind.Null
                            || sha.ValueKind != JsonValueKind.Null
                        : Text(progress, "reference").Length == 0
                            || Hash(progress, "sha256").Length == 0
                )
            )
                throw InvalidRecord();
            completion = new(
                accepted,
                completed,
                pending,
                completed == 0 ? null : reference.GetString(),
                completed == 0 ? null : sha.GetString()
            );
        }
        return new(
            relocationId,
            phase,
            source,
            target is null
                ? null
                : new(
                    target.NodeRid,
                    target.NodeGeneration,
                    target.OwnerId,
                    target.OwnerLeaseGeneration,
                    NonZero(targetValue, "attemptGeneration")
                ),
            new(
                coordinator.NodeRid,
                coordinator.NodeGeneration,
                coordinator.OwnerId,
                coordinator.OwnerLeaseGeneration,
                Text(coordinatorValue, "expectedStoreVersion")
            ),
            Hash(value, "sourceInventorySha256"),
            Number(value, "applicationVersion"),
            completion
        );
    }

    internal static JsonElement? Replace(
        JsonElement? aggregate,
        ZLinkAuthorityRelocation? relocation
    )
    {
        var fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (aggregate is { } current)
        {
            foreach (var property in current.EnumerateObject())
            {
                if (property.Name != "relocation")
                    fields.Add(property.Name, property.Value);
            }
        }
        if (relocation is not null)
        {
            fields.Add(
                "relocation",
                JsonSerializer.SerializeToElement(relocation, ZLinkJsonSerializerOptions.Default)
            );
        }
        return fields.Count == 0 ? null : JsonSerializer.SerializeToElement(fields);
    }

    private static ZLinkAuthorityRelocation.Fence ReadFence(JsonElement value)
    {
        var rid = Text(value, "nodeRid");
        if (rid.Length % 2 != 0 || rid.Length > 510 || !IsLowerHex(rid))
            throw InvalidRecord();
        var nodeGeneration = Number(value, "nodeGeneration");
        var lease = Number(value, "ownerLeaseGeneration");
        if (nodeGeneration == 0 || lease == 0)
            throw InvalidRecord();
        return new(rid, nodeGeneration, Text(value, "ownerId"), lease);
    }

    private static string Text(JsonElement value, string name) =>
        value.ValueKind == JsonValueKind.Object
        && value.TryGetProperty(name, out var field)
        && field.ValueKind == JsonValueKind.String
        && field.GetString() is { Length: > 0 } text
            ? text
            : throw InvalidRecord();

    private static ulong Number(JsonElement value, string name)
    {
        var text = Text(value, name);
        return
            ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var number)
            && text == number.ToString(CultureInfo.InvariantCulture)
            ? number
            : throw InvalidRecord();
    }

    private static ulong NonZero(JsonElement value, string name) =>
        Number(value, name) is var number && number != 0 ? number : throw InvalidRecord();

    private static string Hash(JsonElement value, string name)
    {
        var text = Text(value, name);
        return text.Length == 64 && IsLowerHex(text) ? text : throw InvalidRecord();
    }

    private static bool IsLowerHex(string text) =>
        text.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static InvalidDataException InvalidRecord() =>
        new("The Location Store authority aggregate relocation record is invalid.");
}
