using System.Text.Json;
using System.Text.Json.Serialization;
using Zlink.Framework.Contracts.Handlers;

namespace Zlink.Framework.Runtime.Streams;

internal sealed record AdmissionConnection(
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string NodeRid,
    [property: JsonRequired] ulong NodeLifecycleGeneration,
    [property: JsonRequired] string StreamName,
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string SessionRid
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized() =>
        AdmissionQueueMessages.RequireStreamName(StreamName);
}

internal sealed record AdmissionSession(
    [property: JsonRequired] string StreamName,
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string SessionRid
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized() =>
        AdmissionQueueMessages.RequireStreamName(StreamName);
}

internal sealed record AdmissionDelivery(
    [property: JsonRequired] string StreamName,
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string SessionRid,
    [property: JsonRequired] string Kind,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] int? Position = null,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
        int? EstimatedWaitMs = null,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? Ticket = null,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] int? ExpiresInMs = null
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        AdmissionQueueMessages.RequireStreamName(StreamName);
        if (Kind == "position" && Position is > 0 && EstimatedWaitMs is >= 0)
            return;
        if (
            Kind == "ticket"
            && ExpiresInMs is >= 0
            && Ticket?.Length == ZLinkAdmissionTicket.EncodedSize
            && ZLinkAdmissionTicket.TryDecode(Ticket, out _)
        )
            return;
        throw new JsonException("Invalid admission queue delivery.");
    }
}

internal static class AdmissionQueueMessages
{
    internal const string SpotType = "$zlink.AdmissionQueue";
    internal const string Prefix = "$zlink.admission-queue.";

    internal static string SpotId(string mesh) => $"$zlink.admission-queue/{mesh}";

    internal static void RequireStreamName(string? name)
    {
        if (!ZLinkAdmissionTicket.IsStreamName(name))
            throw new JsonException("Invalid STREAM registration name in admission queue message.");
    }
}

[ZLinkPacket(AdmissionQueueMessages.Prefix + "enqueue")]
internal sealed record AdmissionEnqueue([property: JsonRequired] AdmissionConnection Connection)
    : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        if (Connection is null)
            throw new JsonException("Missing admission queue connection.");
    }
}

internal sealed record AdmissionEnqueued(
    [property: JsonRequired] ulong Epoch,
    [property: JsonRequired] AdmissionDelivery Delivery
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        if (Epoch == 0 || Delivery is null)
            throw new JsonException("Invalid admission queue enqueue response.");
    }
}

[ZLinkPacket(AdmissionQueueMessages.Prefix + "report")]
internal sealed record AdmissionReport(
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string NodeRid,
    [property: JsonRequired] ulong NodeLifecycleGeneration,
    [property: JsonRequired] int Limit,
    [property: JsonRequired] int InFlight,
    [property: JsonRequired] AdmissionSession[] UsedTickets,
    [property: JsonRequired] AdmissionSession[] EndedSessions,
    [property: JsonRequired] ulong[] HeldPermits,
    [property: JsonRequired] ulong ResolvedRequestSeq
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        if (
            Limit <= 0
            || InFlight < 0
            || UsedTickets is null
            || EndedSessions is null
            || HeldPermits is null
            || UsedTickets.Any(static item => item is null)
            || EndedSessions.Any(static item => item is null)
            || HeldPermits.Any(static item => item == 0)
        )
            throw new JsonException("Invalid admission queue node report.");
    }
}

internal sealed record AdmissionReported(
    [property: JsonRequired] ulong Epoch,
    [property: JsonRequired] AdmissionDelivery[] Deliveries
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        if (Epoch == 0 || Deliveries is null || Deliveries.Any(static item => item is null))
            throw new JsonException("Invalid admission queue report response.");
    }
}

[ZLinkPacket(AdmissionQueueMessages.Prefix + "admit-permit")]
internal sealed record AdmissionPermitRequest(
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string NodeRid,
    [property: JsonRequired] ulong NodeLifecycleGeneration,
    [property: JsonRequired] string OwnerId,
    [property: JsonRequired] ulong LeaseGeneration,
    [property: JsonRequired] string StreamName,
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string SessionRid,
    [property: JsonRequired] ulong Epoch,
    [property: JsonRequired] ulong RequestSeq
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        AdmissionQueueMessages.RequireStreamName(StreamName);
        if (string.IsNullOrEmpty(OwnerId) || RequestSeq == 0)
            throw new JsonException("Invalid admission queue permit request.");
    }
}

internal sealed record AdmissionPermitResult(
    [property: JsonRequired] bool Granted,
    [property: JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] ulong? PermitId = null
) : IJsonOnDeserialized
{
    void IJsonOnDeserialized.OnDeserialized()
    {
        if (PermitId is 0 || (Granted && PermitId is null))
            throw new JsonException("Invalid admission queue permit response.");
    }
}

[ZLinkPacket(AdmissionQueueMessages.Prefix + "release-permit")]
internal sealed record AdmissionPermitRelease(
    [property: JsonRequired, JsonConverter(typeof(AdmissionRidConverter))] string NodeRid,
    [property: JsonRequired] ulong NodeLifecycleGeneration,
    [property: JsonRequired] ulong PermitId
);

internal sealed class AdmissionRidConverter : JsonConverter<string>
{
    public override bool HandleNull => true;

    public override string Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        if (
            reader.TokenType == JsonTokenType.String
            && reader.GetString() is { Length: <= 340 } value
            && ZLinkAdmissionTicket.TryDecode(value, out var bytes)
            && bytes.Length is >= 1 and <= byte.MaxValue
        )
            return value;
        throw new JsonException("Expected an unpadded base64url routing ID.");
    }

    public override void Write(
        Utf8JsonWriter writer,
        string value,
        JsonSerializerOptions options
    ) => writer.WriteStringValue(value);
}
