using System.Buffers.Binary;
using System.Text.Json;

namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorRemoteJoinRecoveryCodec
{
    private const uint Magic = 0x5a4c4a52; // ZLJR
    private const byte Version = 1;
    private const int MaximumMetadataBytes = 256 * 1024;
    private const int MaximumMessageBytes = 1024 * 1024;

    internal static byte[] Encode(ZLinkActorRelocationRecoveryRecord value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (value.Request.Request.Length > MaximumMessageBytes
            || value.Reply.Length > MaximumMessageBytes)
            throw new ArgumentOutOfRangeException(nameof(value));
        Validate(value);

        var request = value.Request.Request;
        var reply = value.Reply;
        var metadata = JsonSerializer.SerializeToUtf8Bytes(
            value with
            {
                Request = value.Request with
                {
                    Request = [],
                    HandoffFrames = []
                },
                Reply = []
            });
        if (metadata.Length > MaximumMetadataBytes)
            throw new ArgumentOutOfRangeException(nameof(value));

        var encoded = GC.AllocateUninitializedArray<byte>(
            checked(sizeof(uint) + sizeof(byte)
                    + 3 * sizeof(uint)
                    + metadata.Length + request.Length + reply.Length));
        var offset = 0;
        WriteU32(encoded, ref offset, Magic);
        encoded[offset++] = Version;
        WriteU32(encoded, ref offset, checked((uint)metadata.Length));
        WriteU32(encoded, ref offset, checked((uint)request.Length));
        WriteU32(encoded, ref offset, checked((uint)reply.Length));
        metadata.CopyTo(encoded.AsSpan(offset));
        offset += metadata.Length;
        request.CopyTo(encoded.AsSpan(offset));
        offset += request.Length;
        reply.CopyTo(encoded.AsSpan(offset));
        return encoded;
    }

    internal static ZLinkActorRelocationRecoveryRecord Decode(
        ReadOnlySpan<byte> encoded)
    {
        try
        {
            var offset = 0;
            if (ReadU32(encoded, ref offset) != Magic
                || ReadU8(encoded, ref offset) != Version)
                throw new InvalidDataException();
            var metadataLength = ReadBoundedLength(
                encoded, ref offset, MaximumMetadataBytes);
            var requestLength = ReadBoundedLength(
                encoded, ref offset, MaximumMessageBytes);
            var replyLength = ReadBoundedLength(
                encoded, ref offset, MaximumMessageBytes);
            var metadata = Read(encoded, ref offset, metadataLength);
            var request = Read(encoded, ref offset, requestLength).ToArray();
            var reply = Read(encoded, ref offset, replyLength).ToArray();
            if (offset != encoded.Length)
                throw new InvalidDataException();
            var value = JsonSerializer.Deserialize<
                            ZLinkActorRelocationRecoveryRecord>(metadata)
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
                Request = value.Request with { Request = request },
                Reply = reply
            };
            Validate(restored);
            return restored;
        }
        catch (Exception error) when (error is JsonException
                                      or NotSupportedException
                                      or ArgumentException
                                      or OverflowException
                                      or IndexOutOfRangeException)
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
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException(
                "Canonical Actor Join recovery route is invalid.",
                error);
        }
    }

    private static int ReadBoundedLength(
        ReadOnlySpan<byte> value,
        ref int offset,
        int maximum)
    {
        var length = checked((int)ReadU32(value, ref offset));
        if (length > maximum)
            throw new InvalidDataException();
        return length;
    }

    private static byte ReadU8(ReadOnlySpan<byte> value, ref int offset) =>
        Read(value, ref offset, 1)[0];

    private static uint ReadU32(ReadOnlySpan<byte> value, ref int offset) =>
        BinaryPrimitives.ReadUInt32BigEndian(Read(value, ref offset, 4));

    private static void WriteU32(byte[] value, ref int offset, uint item)
    {
        BinaryPrimitives.WriteUInt32BigEndian(value.AsSpan(offset, 4), item);
        offset += 4;
    }

    private static ReadOnlySpan<byte> Read(
        ReadOnlySpan<byte> value,
        ref int offset,
        int length)
    {
        if (length < 0 || value.Length - offset < length)
            throw new InvalidDataException();
        var result = value.Slice(offset, length);
        offset += length;
        return result;
    }
}
