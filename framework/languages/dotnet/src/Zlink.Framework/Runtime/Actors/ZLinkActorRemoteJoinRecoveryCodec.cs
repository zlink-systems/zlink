using System.Text;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorRemoteJoinRecoveryCodec
{
    private const int MaximumMetadataBytes = 256 * 1024;
    private const int MaximumMessageBytes = 1024 * 1024;
    private static readonly UTF8Encoding Utf8 = new(false, true);

    internal static byte[] Encode(ZLinkActorRelocationRecoveryRecord value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.Request.Request.Length > MaximumMessageBytes
            || value.Reply.Length > MaximumMessageBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        Validate(value);

        var metadata = SerializeMetadata(value);
        if (metadata.Length > MaximumMetadataBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        var request = value.Request;
        return ServiceWirePilotCodec.EncodeZljrRecordV1(new(
            new ServiceWirePilotCodec.ZljrNodeSourceV1(
                Utf8.GetBytes(RoutingId.From(request.SourceNodeRid).ToHex()),
                request.ActorNodeGeneration,
                request.RelocationCoordinatorOwnerId,
                request.RelocationCoordinatorLeaseGeneration),
            new ServiceWirePilotCodec.OperationId(0, 0),
            metadata,
            request.Request,
            value.Reply));
    }

    internal static ZLinkActorRelocationRecoveryRecord Decode(
        ReadOnlySpan<byte> encoded) => Decode(encoded, out _);

    internal static ZLinkActorRelocationRecoveryRecord Decode(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorRelocationSourceFence source)
    {
        source = default!;
        try
        {
            var generated = ServiceWirePilotCodec.DecodeZljrRecordV1(
                encoded.ToArray());
            if (generated.Operation.High != 0 || generated.Operation.Low != 0)
                throw new InvalidDataException();
            var value = JsonSerializer.Deserialize<
                            ZLinkActorRelocationRecoveryRecord>(generated.Metadata)
                        ?? throw new InvalidDataException();
            if (value.Request is null
                || value.Request.Request is null
                || value.Request.HandoffFrames is null
                || value.Reply is null
                || value.Request.Request.Length != 0
                || value.Request.HandoffFrames.Count != 0
                || value.Reply.Length != 0)
                throw new InvalidDataException();
            var restored = value with
            {
                Request = value.Request with { Request = generated.Request },
                Reply = generated.Reply
            };
            Validate(restored);

            var sourceRidHex = Utf8.GetString(generated.Source.NodeRid);
            var sourceRid = RoutingId.FromHex(sourceRidHex);
            if (!sourceRid.ToBytes().SequenceEqual(restored.Request.SourceNodeRid)
                || generated.Source.NodeGeneration
                   != restored.Request.ActorNodeGeneration
                || !StringComparer.Ordinal.Equals(
                    generated.Source.OwnerId,
                    restored.Request.RelocationCoordinatorOwnerId)
                || generated.Source.OwnerLeaseGeneration
                   != restored.Request.RelocationCoordinatorLeaseGeneration)
                throw new InvalidDataException();
            source = new ZLinkActorRelocationSourceFence(
                generated.Source.OwnerId,
                generated.Source.OwnerLeaseGeneration,
                sourceRid,
                generated.Source.NodeGeneration);
            return restored;
        }
        catch (Exception error) when (error is JsonException
                                      or DecoderFallbackException
                                      or NotSupportedException
                                      or ArgumentException
                                      or OverflowException
                                      or IndexOutOfRangeException
                                      or EndOfStreamException)
        {
            throw new InvalidDataException(
                "Canonical Actor Join recovery payload is malformed.",
                error);
        }
    }

    internal static ZLinkActorRelocationRecoveryRecord Decode(
        ReadOnlySpan<byte> operationRecovery,
        ReadOnlySpan<byte> legacyJsonRecovery)
    {
        if (!operationRecovery.IsEmpty && !legacyJsonRecovery.IsEmpty)
            throw new InvalidDataException(
                "Actor Join recovery has conflicting durable representations.");
        if (!operationRecovery.IsEmpty)
            return Decode(operationRecovery);
        if (legacyJsonRecovery.IsEmpty)
            throw new InvalidDataException(
                "Canonical Actor Join recovery metadata is unavailable.");
        try
        {
            var value = JsonSerializer.Deserialize<
                            ZLinkActorRelocationRecoveryRecord>(
                            legacyJsonRecovery)
                        ?? throw new InvalidDataException();
            Validate(value);
            return value;
        }
        catch (Exception error) when (error is JsonException
                                      or NotSupportedException
                                      or ArgumentException)
        {
            throw new InvalidDataException(
                "Legacy Actor Join recovery payload is malformed.",
                error);
        }
    }

    private static byte[] SerializeMetadata(
        ZLinkActorRelocationRecoveryRecord value)
    {
        var projection = JsonSerializer.SerializeToElement(value with
        {
            Request = value.Request with
            {
                Request = [],
                HandoffFrames = []
            },
            Reply = []
        });
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            foreach (var property in projection.EnumerateObject())
            {
                if (!property.NameEquals(nameof(value.Request)))
                {
                    property.WriteTo(writer);
                    continue;
                }
                writer.WritePropertyName(property.Name);
                writer.WriteStartObject();
                foreach (var requestProperty in property.Value.EnumerateObject())
                {
                    if (!requestProperty.NameEquals(
                            nameof(value.Request.TargetAttemptGeneration)))
                        requestProperty.WriteTo(writer);
                }
                writer.WriteEndObject();
            }
            writer.WriteEndObject();
        }
        return stream.ToArray();
    }

    private static void Validate(ZLinkActorRelocationRecoveryRecord value)
    {
        if (value is null || value.Request is null
            || value.Request.Request is null
            || value.Request.HandoffFrames is null
            || value.Reply is null)
            throw new InvalidDataException(
                "Canonical Actor Join recovery fields are missing.");
        var request = value.Request;
        if (string.IsNullOrWhiteSpace(request.ActorId)
            || string.IsNullOrWhiteSpace(request.ActorType)
            || string.IsNullOrWhiteSpace(request.HandoffId)
            || string.IsNullOrWhiteSpace(request.SourceSpotId)
            || request.SourceNodeRid is not { Length: > 0 }
            || request.ActorGeneration == 0
            || request.ActorAuthorityOwnerGeneration == 0
            || request.RelocationAggregateId == Guid.Empty
            || request.RelocationAggregateGeneration == 0
            || request.RelocationInventoryDigest is not { Length: 32 }
            || string.IsNullOrWhiteSpace(request.RequestContentType)
            || string.IsNullOrWhiteSpace(value.TargetSpotId)
            || value.TargetNodeRid is not { Length: > 0 }
            || value.TargetNodeGeneration == 0
            || value.TargetSpotGeneration == 0
            || value.TargetAuthorityOwnerGeneration == 0
            || request.TargetNodeRid is not { Length: > 0 }
            || !request.TargetNodeRid.AsSpan().SequenceEqual(
                value.TargetNodeRid)
            || request.TargetNodeGeneration != value.TargetNodeGeneration
            || request.TargetSpotGeneration != value.TargetSpotGeneration
            || request.TargetAuthorityOwnerGeneration
               != value.TargetAuthorityOwnerGeneration
            || request.ActorNodeGeneration == 0
            || request.ExpectedOwnerLeaseGeneration == 0
            || string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorOwnerId)
            || request.RelocationCoordinatorLeaseGeneration == 0
            || request.RelocationCoordinatorNodeRid is not { Length: > 0 }
            || request.RelocationCoordinatorNodeGeneration == 0
            || string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorExpectedAuthorityStoreVersion)
            || (value.OperationIdHigh == 0 && value.OperationIdLow == 0
                && (!string.IsNullOrEmpty(value.ReplyContentType)
                    || value.Reply.Length != 0))
            || ((value.OperationIdHigh != 0 || value.OperationIdLow != 0)
                && string.IsNullOrWhiteSpace(value.ReplyContentType)))
            throw new InvalidDataException(
                "Canonical Actor Join recovery identity is invalid.");

        try
        {
            _ = RoutingId.From(request.SourceNodeRid);
            _ = RoutingId.From(value.TargetNodeRid);
            _ = RoutingId.From(request.RelocationCoordinatorNodeRid);
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException(
                "Canonical Actor Join recovery route is invalid.",
                error);
        }
    }
}
