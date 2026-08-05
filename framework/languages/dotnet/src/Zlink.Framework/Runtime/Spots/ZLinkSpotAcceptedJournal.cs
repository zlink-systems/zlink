using System.Text;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Backend.DotNet;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotAcceptedJournalRecord(
    RoutingId? SourceNodeRid,
    ulong SourceNodeGeneration,
    ZLinkServiceWireCodec.RequestSourceFence? RequestSource,
    string? SpotId,
    ulong? RequestSequence,
    ulong ReplyRouteId,
    MeshOperationId OperationId,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    byte MessageFollowHopCount,
    ZLinkMessageMetadata Metadata,
    IReadOnlyList<ReadOnlyMemory<byte>> Parts);

internal static class ZLinkSpotAcceptedJournal
{
    private const uint Magic = 0x5a4a5231; // ZJR1
    private const ushort Version = 6;
    private const int MaxRecordBytes = 64 * 1024 * 1024;
    private const int MaxParts = 65_536;

    internal static byte[] CaptureOrDispose(
        ZLinkBackendRouteReceived received,
        ulong replyRouteId = 0)
    {
        try
        {
            return Encode(received, replyRouteId);
        }
        catch
        {
            received.Dispose();
            throw;
        }
    }

    // Computes the reservation size without creating the relocation record.
    // The record itself is materialized only after the serial queue has sealed.
    internal static int MeasureEncodedLength(
        ZLinkBackendRouteReceived received,
        ulong replyRouteId = 0)
    {
        ArgumentNullException.ThrowIfNull(received);
        if (received.OperationId == default
            || received.TargetNodeGeneration == 0
            || received.AuthorityOwnerGeneration == 0
            || received.OwnerLeaseGeneration == 0)
            throw new InvalidOperationException(
                "An accepted Spot journal record requires an exact operation and authority fence.");
        if (received.RequestSource is not { } requestSource
            || received.SourceNodeRid is not { } sourceNodeRid
            || requestSource.NodeRid != sourceNodeRid
            || requestSource.NodeGeneration != received.SourceNodeGeneration
            || string.IsNullOrWhiteSpace(requestSource.OwnerId)
            || requestSource.LeaseGeneration == 0)
            throw new InvalidOperationException(
                "An accepted Spot journal record requires the exact ingress request-source fence.");
        if (replyRouteId != 0
            && (received.RequestSeq != replyRouteId
                || received.OperationId.Low != replyRouteId))
            throw new InvalidOperationException(
                "An accepted Spot request must preserve the source-owned operation correlation as its reply route.");
        if (replyRouteId == 0 && received.CanReply)
            throw new InvalidOperationException(
                "An accepted Spot request cannot omit its source-owned reply route.");
        if (received.Parts.Count > MaxParts)
            throw new InvalidOperationException(
                "An accepted Spot journal record contains too many message parts.");

        var length = 4 + 2; // magic + version
        length = checked(length
            + 1
            + 4 + sourceNodeRid.Size
            + 8
            + 4 + Encoding.UTF8.GetByteCount(requestSource.OwnerId)
            + 8
            + 1
            + (string.IsNullOrEmpty(received.SpotId)
                ? 0
                : 4 + Encoding.UTF8.GetByteCount(received.SpotId))
            + 1
            + (received.RequestSeq.HasValue ? 8 : 0)
            + 8
            + 8 + 8
            + 8 + 8 + 8
            + 1);
        var metadataLength = ZLinkMeshMetadataCodec.MeasureEncodedLength(
            received.Metadata);
        length = checked(length + 4 + metadataLength + 4);
        foreach (var part in received.Parts)
            length = checked(length + 4 + part.Size);
        if (length > MaxRecordBytes)
            throw new InvalidOperationException(
                "An accepted Spot journal record cannot exceed 64 MiB.");
        return length;
    }

    internal static byte[] Encode(
        ZLinkBackendRouteReceived received,
        ulong replyRouteId = 0)
    {
        ArgumentNullException.ThrowIfNull(received);
        if (received.OperationId == default
            || received.TargetNodeGeneration == 0
            || received.AuthorityOwnerGeneration == 0
            || received.OwnerLeaseGeneration == 0)
            throw new InvalidOperationException(
                "An accepted Spot journal record requires an exact operation and authority fence.");
        if (received.RequestSource is not { } requestSource
            || received.SourceNodeRid is not { } sourceNodeRid
            || requestSource.NodeRid != sourceNodeRid
            || requestSource.NodeGeneration != received.SourceNodeGeneration
            || string.IsNullOrWhiteSpace(requestSource.OwnerId)
            || requestSource.LeaseGeneration == 0)
            throw new InvalidOperationException(
                "An accepted Spot journal record requires the exact ingress request-source fence.");
        if (replyRouteId != 0
            && (received.RequestSeq != replyRouteId
                || received.OperationId.Low != replyRouteId))
            throw new InvalidOperationException(
                "An accepted Spot request must preserve the source-owned operation correlation as its reply route.");
        if (replyRouteId == 0 && received.CanReply)
            throw new InvalidOperationException(
                "An accepted Spot request cannot omit its source-owned reply route.");
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        WriteRoutingId(writer, received.SourceNodeRid);
        writer.Write(received.SourceNodeGeneration);
        WriteText(writer, requestSource.OwnerId);
        writer.Write(requestSource.LeaseGeneration);
        WriteSpotId(writer, received.SpotId);
        writer.Write(received.RequestSeq.HasValue);
        if (received.RequestSeq is { } requestSequence)
            writer.Write(requestSequence);
        writer.Write(replyRouteId);
        writer.Write(received.OperationId.High);
        writer.Write(received.OperationId.Low);
        writer.Write(received.TargetNodeGeneration);
        writer.Write(received.AuthorityOwnerGeneration);
        writer.Write(received.OwnerLeaseGeneration);
        writer.Write(received.MessageFollowHopCount);
        WriteBytes(writer, ZLinkMeshMetadataCodec.Encode(received.Metadata).Span);
        if (received.Parts.Count > MaxParts)
            throw new InvalidOperationException(
                "An accepted Spot journal record contains too many message parts.");
        writer.Write(received.Parts.Count);
        foreach (var part in received.Parts)
            WriteBytes(writer, part.AsReadOnlySpan());
        writer.Flush();
        if (stream.Length > MaxRecordBytes)
            throw new InvalidOperationException(
                "An accepted Spot journal record cannot exceed 64 MiB.");
        return stream.ToArray();
    }

    internal static ZLinkSpotAcceptedJournalRecord Decode(ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length is <= 0 or > MaxRecordBytes)
            throw new InvalidDataException(
                "The accepted Spot journal record size is invalid.");
        using var stream = new MemoryStream(encoded.ToArray(), writable: false);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        if (reader.ReadUInt32() != Magic)
            throw new InvalidDataException(
                "The accepted Spot journal record header is invalid.");
        var version = reader.ReadUInt16();
        if (version is not (4 or 5 or Version))
            throw new InvalidDataException(
                "The accepted Spot journal record header is invalid.");
        var sourceNodeRid = ReadRoutingId(reader);
        var sourceNodeGeneration = version >= 5 ? reader.ReadUInt64() : 0;
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null;
        if (version >= 6)
        {
            var sourceOwnerId = ReadText(reader);
            var sourceOwnerLeaseGeneration = reader.ReadUInt64();
            if (sourceNodeRid is not { } exactSourceNodeRid
                || sourceNodeGeneration == 0
                || string.IsNullOrWhiteSpace(sourceOwnerId)
                || sourceOwnerLeaseGeneration == 0)
                throw new InvalidDataException(
                    "The accepted Spot journal request-source fence is invalid.");
            requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
                sourceOwnerId,
                sourceOwnerLeaseGeneration,
                exactSourceNodeRid,
                sourceNodeGeneration);
        }
        var spotId = ReadSpotId(reader);
        var requestSequence = reader.ReadBoolean()
            ? reader.ReadUInt64()
            : (ulong?)null;
        var replyRouteId = reader.ReadUInt64();
        var operationId = new MeshOperationId(
            reader.ReadUInt64(),
            reader.ReadUInt64());
        var targetNodeGeneration = reader.ReadUInt64();
        var authorityOwnerGeneration = reader.ReadUInt64();
        var ownerLeaseGeneration = reader.ReadUInt64();
        var messageFollowHopCount = reader.ReadByte();
        if (operationId == default
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0
            || replyRouteId != 0
            && (requestSequence != replyRouteId
                || operationId.Low != replyRouteId)
            || messageFollowHopCount > 8)
            throw new InvalidDataException(
                "The accepted Spot journal authority fence is invalid.");
        var metadataFrame = ReadBytes(reader);
        if (!ZLinkMeshMetadataCodec.TryDecode(metadataFrame, out var metadata))
            throw new InvalidDataException(
                "The accepted Spot journal metadata is invalid.");
        var partCount = reader.ReadInt32();
        if (partCount < 0 || partCount > MaxParts)
            throw new InvalidDataException(
                "The accepted Spot journal part count is invalid.");
        var parts = new ReadOnlyMemory<byte>[partCount];
        for (var index = 0; index < parts.Length; index++)
            parts[index] = ReadBytes(reader);
        if (stream.Position != stream.Length)
            throw new InvalidDataException(
                "The accepted Spot journal record contains trailing bytes.");
        return new ZLinkSpotAcceptedJournalRecord(
            sourceNodeRid,
            sourceNodeGeneration,
            requestSource,
            spotId,
            requestSequence,
            replyRouteId,
            operationId,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            messageFollowHopCount,
            metadata,
            parts);
    }

    private static void WriteRoutingId(BinaryWriter writer, RoutingId? value)
    {
        writer.Write(value.HasValue);
        if (value is { } routingId)
            WriteBytes(writer, routingId.ToBytes());
    }

    private static RoutingId? ReadRoutingId(BinaryReader reader)
    {
        return reader.ReadBoolean()
            ? RoutingId.From(ReadBytes(reader))
            : null;
    }

    private static void WriteSpotId(BinaryWriter writer, string? value)
    {
        // Node-originated Instance Spot intents have no source Spot. The
        // service wire represents that optional value as an empty string;
        // preserve it in the durable journal as an absent Spot ID.
        var present = !string.IsNullOrEmpty(value);
        writer.Write(present);
        if (!present) return;
        WriteBytes(writer, Encoding.UTF8.GetBytes(
            ZLinkSpotId.Require(value!, nameof(value))));
    }

    private static void WriteText(BinaryWriter writer, string value) =>
        WriteBytes(writer, Encoding.UTF8.GetBytes(value));

    private static string ReadText(BinaryReader reader)
    {
        try
        {
            return new UTF8Encoding(false, true).GetString(ReadBytes(reader));
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "The accepted Spot journal text is invalid.", error);
        }
    }

    private static string? ReadSpotId(BinaryReader reader)
    {
        if (!reader.ReadBoolean()) return null;
        var encoded = ReadBytes(reader);
        try
        {
            var value = new UTF8Encoding(false, true).GetString(encoded);
            return ZLinkSpotId.IsValid(value)
                ? value
                : throw new InvalidDataException("The accepted Spot ID is invalid.");
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException("The accepted Spot ID is not valid UTF-8.", error);
        }
    }

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        writer.Write(value.Length);
        writer.Write(value);
    }

    private static byte[] ReadBytes(BinaryReader reader)
    {
        var length = reader.ReadInt32();
        if (length < 0 || length > MaxRecordBytes)
            throw new InvalidDataException(
                "An accepted Spot journal byte field exceeds its bound.");
        var value = reader.ReadBytes(length);
        if (value.Length != length)
            throw new EndOfStreamException();
        return value;
    }
}
