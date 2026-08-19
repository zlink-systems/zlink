using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Contracts.Locations;

// Canonical epoch-millisecond timestamp encoding shared by the descriptor
// records' `updatedAtEpochMs` field (21-location-runtime.md#2.4: "timestamps
// are strings carrying a Unix epoch millisecond value"). Scoped as a
// property-level converter (like the FrameworkSigned64/Unsigned64
// converters it sits beside) rather than added to
// ZLinkJsonSerializerOptions.Default, since that options object also
// serializes unrelated DTOs whose DateTimeOffset encoding isn't part of
// this contract.
internal sealed class ZLinkEpochMillisecondsJsonConverter : JsonConverter<DateTimeOffset>
{
    public override DateTimeOffset Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.String)
            throw new JsonException("Epoch millisecond timestamps must be decimal strings.");
        var encoded = reader.GetString();
        if (encoded is null
            || !long.TryParse(
                encoded,
                NumberStyles.AllowLeadingSign,
                CultureInfo.InvariantCulture,
                out var value)
            || !string.Equals(
                encoded,
                value.ToString(CultureInfo.InvariantCulture),
                StringComparison.Ordinal))
            throw new JsonException("Epoch millisecond timestamp string is not canonical.");
        return DateTimeOffset.FromUnixTimeMilliseconds(value);
    }

    public override void Write(
        Utf8JsonWriter writer,
        DateTimeOffset value,
        JsonSerializerOptions options) =>
        writer.WriteStringValue(
            value.ToUnixTimeMilliseconds().ToString(CultureInfo.InvariantCulture));
}

/// <summary>
///     One MeshNode's published physical identity: endpoint plus the whole
///     immutable ChannelName membership with its mutable weights. One
///     descriptor per (MeshName, Rid); never one row per channel
///     (06-location-store §4). Field names/types/converters below are the
///     canonical opaque-record contract (21-location-runtime.md#2.4's
///     MeshNode descriptor table) -- this type is serialized directly as
///     the descriptor sub-object.
/// </summary>
internal sealed record ZLinkMeshNodeDescriptor(
    string MeshName,
    [property: JsonPropertyName("routingIdHex")] RoutingId Rid,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong LifecycleGeneration,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong DescriptorRevision,
    string Endpoint,
    IReadOnlyDictionary<string, int> ChannelWeights,
    string SecurityIdentity,
    string OwnerId,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter))]
    long LeaseGeneration,
    [property: JsonPropertyName("updatedAtEpochMs")]
    [property: JsonConverter(typeof(ZLinkEpochMillisecondsJsonConverter))]
    DateTimeOffset UpdatedAt)
{
    [JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter))]
    public long ApplicationVersion { get; init; }

    public IReadOnlyList<ZLinkObjectCapability> ObjectCapabilities { get; init; }
        = Array.Empty<ZLinkObjectCapability>();

    public string? MaintenanceWave { get; init; }

    [JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkFrameworkRuntimeState>))]
    public ZLinkFrameworkRuntimeState State { get; init; }

    public ZLinkMeshNodeObjectRole ObjectRole { get; init; }

    public string? EntrySpotId { get; init; }

    public int PlacementWeight { get; init; } = 100;

    public ZLinkPlacementCapacity Capacity { get; init; }
        = new(
            new ZLinkPopulationCapacity(0, 0, 0),
            new ZLinkPopulationCapacity(0, 0, 0),
            Array.Empty<ZLinkSpotTypeCapacity>());

    public ZLinkActivationConcurrency ActivationConcurrency { get; init; }
        = new(0, 128);
}

// Embedded as the authority record's allocation.descriptor field
// (21-location-runtime.md#2.4); the wire field name is routingIdHex, not
// the C# property name Rid.
internal readonly record struct ZLinkMeshNodeDescriptorKey(
    string MeshName,
    [property: JsonPropertyName("routingIdHex")] RoutingId Rid);

// Field names/types/converters below are the canonical opaque-record
// contract (21-location-runtime.md#2.4's ClientServer server descriptor
// table).
internal sealed record ZLinkClientServerServerDescriptor(
    string ChannelName,
    [property: JsonPropertyName("serverRoutingIdHex")] RoutingId ServerRid,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong LifecycleGeneration,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong DescriptorRevision,
    string Endpoint,
    int Weight,
    [property: JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkFrameworkRuntimeState>))]
    ZLinkFrameworkRuntimeState State,
    string SecurityIdentity,
    string OwnerId,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter))]
    long LeaseGeneration,
    [property: JsonPropertyName("updatedAtEpochMs")]
    [property: JsonConverter(typeof(ZLinkEpochMillisecondsJsonConverter))]
    DateTimeOffset UpdatedAt);

internal readonly record struct ZLinkClientServerServerDescriptorKey(
    string ChannelName,
    RoutingId ServerRid);

// Same fields as ZLinkClientServerServerDescriptor minus Weight
// (21-location-runtime.md#2.4's fanout publisher descriptor table).
internal sealed record ZLinkFanoutPublisherDescriptor(
    string ChannelName,
    [property: JsonPropertyName("publisherRoutingIdHex")] RoutingId PublisherRid,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong LifecycleGeneration,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
    ulong DescriptorRevision,
    string Endpoint,
    [property: JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkFrameworkRuntimeState>))]
    ZLinkFrameworkRuntimeState State,
    string SecurityIdentity,
    string OwnerId,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter))]
    long LeaseGeneration,
    [property: JsonPropertyName("updatedAtEpochMs")]
    [property: JsonConverter(typeof(ZLinkEpochMillisecondsJsonConverter))]
    DateTimeOffset UpdatedAt);

internal readonly record struct ZLinkFanoutPublisherDescriptorKey(
    string ChannelName,
    RoutingId PublisherRid);

[JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkMeshNodeObjectRole>))]
internal enum ZLinkMeshNodeObjectRole
{
    None = 0,
    Client = 1,
    Server = 2
}

[JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkObjectMaintenancePolicyKind>))]
internal enum ZLinkObjectMaintenancePolicyKind
{
    Unspecified = 0,
    Disabled = 1,
    Recreate = 2,
    Snapshot = 3
}

internal sealed record ZLinkObjectCapability(
    ZLinkPlacementObjectKind ObjectKind,
    string StableType,
    ZLinkObjectMaintenancePolicyKind Policy,
    bool HasSnapshotAdapter,
    int Limit);

internal sealed record ZLinkPopulationCapacity(
    int Active,
    int Reserved,
    int Limit);

internal sealed record ZLinkSpotTypeCapacity(
    ZLinkPlacementObjectKind ObjectKind,
    string StableType,
    int Active,
    int Reserved,
    int Limit);

internal sealed record ZLinkPlacementCapacity(
    ZLinkPopulationCapacity Actors,
    ZLinkPopulationCapacity Spots,
    IReadOnlyList<ZLinkSpotTypeCapacity> SpotTypes);

internal sealed record ZLinkActivationConcurrency(
    int Active,
    int Limit);
