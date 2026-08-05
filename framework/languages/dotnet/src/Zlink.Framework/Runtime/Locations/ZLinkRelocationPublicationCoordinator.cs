using System.Text;
using System.Buffers.Binary;
using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationPublicationRequest(
    ZLinkAuthorityKey AuthorityKey,
    string ExpectedStoreVersion,
    ZLinkAuthorityGenerationTransition GenerationTransition,
    string TargetOwnerId,
    long TargetOwnerLeaseGeneration,
    ReadOnlyMemory<byte> ApplicationAuthorityPayload,
    ZLinkRelocationCapacityFence? RelocationCapacityFence,
    ZLinkRelocationEnvelope Envelope);

internal sealed record ZLinkPublishedRelocation(
    ZLinkAuthoritySnapshot Authority,
    ZLinkRelocationStored Relocation,
    ZLinkRelocationEnvelope Envelope);

internal sealed record ZLinkPreparedRelocation(
    ZLinkRelocationStored Relocation,
    ZLinkRelocationEnvelope Envelope)
{
    internal ZLinkRelocationManifestReference Reference => new(
        Relocation.Reference,
        Relocation.ChecksumCrc32c,
        Envelope.AggregateId,
        Envelope.AggregateGeneration,
        Envelope.InventoryDigest);
}

internal sealed record ZLinkRelocationManifestReference(
    string Reference,
    uint ChecksumCrc32c,
    Guid AggregateId,
    ulong AggregateGeneration,
    ReadOnlyMemory<byte> InventoryDigest);

internal sealed class ZLinkRelocationDataLostException(string message)
    : IOException(message);

internal sealed class ZLinkRelocationPublicationConflictException(
    ZLinkAuthorityReadResult current)
    : InvalidOperationException("The relocation authority publication conflicted.")
{
    internal ZLinkAuthorityReadResult Current { get; } = current;
}

internal sealed class ZLinkRelocationPublicationCoordinator(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore)
{
    private static readonly TimeSpan Retention = TimeSpan.FromHours(24);

    internal async ValueTask<ZLinkPublishedRelocation> PublishAsync(
        ZLinkRelocationPublicationRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        cancellationToken.ThrowIfCancellationRequested();

        var prepared = await PrepareAsync(request.Envelope, cancellationToken)
            .ConfigureAwait(false);
        return await PublishPreparedAsync(request, prepared, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkPreparedRelocation> PrepareAsync(
        ZLinkRelocationEnvelope envelope,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        cancellationToken.ThrowIfCancellationRequested();
        var tree = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                envelope,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var stored = tree.Root;
        try
        {
            return new ZLinkPreparedRelocation(stored, envelope);
        }
        catch
        {
            await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
            throw;
        }
    }

    internal ValueTask DiscardPreparedAsync(
        ZLinkPreparedRelocation prepared)
    {
        ArgumentNullException.ThrowIfNull(prepared);
        return DeleteOrphanAsync(prepared.Relocation.Reference);
    }

    internal async ValueTask<ZLinkPublishedRelocation> PublishPreparedAsync(
        ZLinkRelocationPublicationRequest request,
        ZLinkPreparedRelocation prepared,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(prepared);
        ValidateRequest(request);
        ValidatePrepared(request.Envelope, prepared);
        if (cancellationToken.IsCancellationRequested)
        {
            await DeleteOrphanAsync(prepared.Relocation.Reference)
                .ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
        }

        var stored = prepared.Relocation;
        byte[] publishedPayload;
        try
        {
            await ZLinkRelocationTreeStore.RenewTreeAsync(
                    relocationStore,
                    stored.Reference,
                    stored.ChecksumCrc32c,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!request.Envelope.CanonicalLogicalStream.IsEmpty)
            {
                if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        request.ApplicationAuthorityPayload.Span,
                        out var canonical)
                    || canonical.RelocationReference != stored.Reference
                    || canonical.RelocationChecksumCrc32c
                       != stored.ChecksumCrc32c)
                    throw new InvalidDataException(
                        "Canonical relocation publication payload does not match its immutable root.");
                publishedPayload =
                    request.ApplicationAuthorityPayload.ToArray();
            }
            else
            {
                publishedPayload = ZLinkRelocationAuthorityPayloadCodec.Encode(
                    new ZLinkRelocationAuthorityPayload(
                        stored.Reference,
                        stored.ChecksumCrc32c,
                        request.Envelope.AggregateId,
                        request.Envelope.AggregateGeneration,
                        request.Envelope.InventoryDigest,
                        request.TargetOwnerId,
                        request.TargetOwnerLeaseGeneration,
                        request.ApplicationAuthorityPayload));
            }
        }
        catch
        {
            await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
            throw;
        }

        try
        {
            var result = await authorityStore.CompareExchangeAuthorityAsync(
                    request.AuthorityKey,
                    request.ExpectedStoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        publishedPayload,
                        request.GenerationTransition,
                        request.GenerationTransition
                        == ZLinkAuthorityGenerationTransition.Preserve
                            ? null
                            : new ZLinkLocationOwnerToken(
                                request.TargetOwnerId,
                                request.TargetOwnerLeaseGeneration),
                        request.RelocationCapacityFence),
                    cancellationToken)
                .ConfigureAwait(false);
            switch (result)
            {
                case ZLinkAuthorityCompareExchangeResult.Stored success:
                    ValidatePublishedSnapshot(success.Snapshot, request, stored);
                    return new ZLinkPublishedRelocation(
                        success.Snapshot,
                        stored,
                        request.Envelope);

                case ZLinkAuthorityCompareExchangeResult.Conflict conflict:
                    if (TryReconcilePublished(
                            conflict.Current,
                            request,
                            stored,
                            out var reconciled))
                        return new ZLinkPublishedRelocation(
                            reconciled,
                            stored,
                            request.Envelope);
                    await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
                    throw new ZLinkRelocationPublicationConflictException(
                        conflict.Current);

                case ZLinkAuthorityCompareExchangeResult.GenerationExhausted:
                    await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
                    throw new ZLinkAuthorityGenerationExhaustedException(
                        "publishing relocation authority");

                default:
                    await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
                    throw new InvalidOperationException(
                        "The authority store returned an invalid relocation publication result.");
            }
        }
        catch (ZLinkRelocationPublicationConflictException)
        {
            throw;
        }
        catch
        {
            // A provider exception or waiter cancellation can happen after the
            // CAS committed. Reconcile against the authority before deleting the
            // immutable root, because deleting a published root is data loss.
            var current = await TryReadAuthorityWithoutCancellationAsync(
                    request.AuthorityKey)
                .ConfigureAwait(false);
            if (current is not null
                && TryReconcilePublished(
                    current,
                    request,
                    stored,
                    out var reconciled))
            {
                return new ZLinkPublishedRelocation(
                    reconciled,
                    stored,
                    request.Envelope);
            }

            await DeleteOrphanAsync(stored.Reference).ConfigureAwait(false);
            throw;
        }
    }

    internal async ValueTask<ZLinkRelocationEnvelope> ReadPreparedAsync(
        ZLinkRelocationManifestReference reference,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(reference);
        ValidateManifestReference(reference);
        var envelope = await ZLinkRelocationTreeStore.GetAsync(
                relocationStore,
                reference.Reference,
                reference.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        return ValidateRoot(envelope, reference);
    }

    internal async ValueTask<ZLinkPublishedRelocation?> RecoverAsync(
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken = default)
    {
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is ZLinkAuthorityReadResult.Missing)
            return null;
        var authority = ((ZLinkAuthorityReadResult.Found)read).Snapshot;
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                authority.Payload.Span,
                out var publication))
            return null;
        if (!string.Equals(
                publication.TargetOwnerId,
                authority.OwnerId,
                StringComparison.Ordinal)
            || publication.TargetOwnerLeaseGeneration
            != authority.OwnerLeaseGeneration)
            throw new ZLinkRelocationDataLostException(
                $"Published relocation authority '{key.Value}' has an invalid owner fence.");

        var reference = new ZLinkRelocationManifestReference(
            publication.Reference,
            publication.ChecksumCrc32c,
            publication.AggregateId,
            publication.AggregateGeneration,
            publication.InventoryDigest);
        var envelope = await ReadPreparedAsync(reference, cancellationToken)
            .ConfigureAwait(false);

        return new ZLinkPublishedRelocation(
            authority,
            new ZLinkRelocationStored(
                publication.Reference,
                publication.ChecksumCrc32c,
                default,
                authority.StoreNow),
            envelope);
    }

    internal async ValueTask<ZLinkAuthoritySnapshot?> ReleasePublishedAsync(
        ZLinkAuthorityKey key,
        string reference,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(key.Value);
        ArgumentException.ThrowIfNullOrWhiteSpace(reference);
        var read = await authorityStore.ReadAuthorityAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || !string.Equals(
                publication.Reference,
                reference,
                StringComparison.Ordinal))
            return null;
        var released = await authorityStore.CompareExchangeAuthorityAsync(
                key,
                found.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    publication.ApplicationPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                cancellationToken)
            .ConfigureAwait(false);
        if (released is ZLinkAuthorityCompareExchangeResult.Stored stored)
        {
            await DeleteOrphanAsync(reference).ConfigureAwait(false);
            return stored.Snapshot;
        }
        return null;
    }

    private static ZLinkRelocationEnvelope ValidateRoot(
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationManifestReference reference)
    {
        if (!envelope.CanonicalLogicalStream.IsEmpty
            || (envelope.Participants.Count > 0
                && envelope.Participants.All(
                    participant =>
                        ZLinkCanonicalParticipantRecoveryCodec.IsEncoded(
                            participant.RecoveryPayload.Span))))
        {
            if (envelope.AggregateId != reference.AggregateId)
                throw new ZLinkRelocationDataLostException(
                    $"Relocation root '{reference.Reference}' does not match its manifest.");
            return envelope with
            {
                AggregateGeneration = reference.AggregateGeneration
            };
        }
        if (envelope.AggregateId != reference.AggregateId
            || envelope.AggregateGeneration != reference.AggregateGeneration
            || !envelope.InventoryDigest.Span.SequenceEqual(
                reference.InventoryDigest.Span))
            throw new ZLinkRelocationDataLostException(
                $"Relocation root '{reference.Reference}' does not match its manifest.");
        return envelope;
    }

    private async ValueTask<ZLinkAuthorityReadResult?> TryReadAuthorityWithoutCancellationAsync(
        ZLinkAuthorityKey key)
    {
        try
        {
            return await authorityStore.ReadAuthorityAsync(key, CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
    }

    private async ValueTask DeleteOrphanAsync(string reference)
    {
        try
        {
            await ZLinkRelocationTreeStore.DeleteTreeAsync(
                    relocationStore,
                    reference,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // The fixed 24-hour retention remains the final orphan cleanup.
        }
    }

    private static void ValidateRequest(ZLinkRelocationPublicationRequest request)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(request.AuthorityKey.Value);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.TargetOwnerId);
        if (request.TargetOwnerLeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "The target owner lease generation must be positive.");
        if (request.GenerationTransition
                == ZLinkAuthorityGenerationTransition.NewOwner
            && request.RelocationCapacityFence is null
            || request.GenerationTransition
                != ZLinkAuthorityGenerationTransition.NewOwner
            && request.RelocationCapacityFence is not null)
            throw new ArgumentException(
                "Only NewOwner publication requires a relocation capacity fence.",
                nameof(request));
        if (request.ApplicationAuthorityPayload.Length > 1024 * 1024)
            throw new ArgumentOutOfRangeException(
                nameof(request),
                "The application authority payload cannot exceed 1 MiB.");
    }

    private static void ValidatePrepared(
        ZLinkRelocationEnvelope requested,
        ZLinkPreparedRelocation prepared)
    {
        var reference = prepared.Reference;
        ValidateManifestReference(reference);
        if (requested.AggregateId != reference.AggregateId
            || requested.AggregateGeneration != reference.AggregateGeneration
            || !requested.InventoryDigest.Span.SequenceEqual(
                reference.InventoryDigest.Span))
            throw new ArgumentException(
                "The prepared relocation does not match the publication request.",
                nameof(prepared));
        if (!ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(requested)
                .AsSpan().SequenceEqual(
                    ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(
                        prepared.Envelope)))
            throw new ArgumentException(
                "The prepared relocation root does not match the publication request.",
                nameof(prepared));
    }

    private static void ValidateManifestReference(
        ZLinkRelocationManifestReference reference)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(reference.Reference);
        if (reference.AggregateId == Guid.Empty
            || reference.AggregateGeneration is 0 or > long.MaxValue
            || reference.InventoryDigest.Length != 32)
            throw new ArgumentException(
                "The relocation manifest reference is invalid.",
                nameof(reference));
    }

    private static bool TryReconcilePublished(
        ZLinkAuthorityReadResult current,
        ZLinkRelocationPublicationRequest request,
        ZLinkRelocationStored stored,
        out ZLinkAuthoritySnapshot snapshot)
    {
        snapshot = null!;
        if (current is not ZLinkAuthorityReadResult.Found found
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || !string.Equals(
                publication.Reference,
                stored.Reference,
                StringComparison.Ordinal)
            || publication.ChecksumCrc32c != stored.ChecksumCrc32c
            || publication.AggregateId != request.Envelope.AggregateId
            || publication.AggregateGeneration
            != request.Envelope.AggregateGeneration
            || !publication.InventoryDigest.Span.SequenceEqual(
                request.Envelope.InventoryDigest.Span))
            return false;
        ValidatePublishedSnapshot(found.Snapshot, request, stored);
        snapshot = found.Snapshot;
        return true;
    }

    private static void ValidatePublishedSnapshot(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkRelocationPublicationRequest request,
        ZLinkRelocationStored stored)
    {
        if (snapshot.ObjectGeneration is 0 or > long.MaxValue
            || snapshot.AuthorityOwnerGeneration is 0 or > long.MaxValue)
            throw new InvalidDataException(
                "Authority Store returned an invalid generation.");
        if (!string.Equals(
                snapshot.OwnerId,
                request.TargetOwnerId,
                StringComparison.Ordinal)
            || snapshot.OwnerLeaseGeneration != request.TargetOwnerLeaseGeneration)
            throw new InvalidDataException(
                "Authority Store did not publish the requested target owner fence.");
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var publication)
            || !string.Equals(
                publication.Reference,
                stored.Reference,
                StringComparison.Ordinal)
            || publication.ChecksumCrc32c != stored.ChecksumCrc32c)
            throw new InvalidDataException(
                "Authority Store did not preserve the relocation publication.");
    }
}

internal sealed record ZLinkRelocationAuthorityPayload(
    string Reference,
    uint ChecksumCrc32c,
    Guid AggregateId,
    ulong AggregateGeneration,
    ReadOnlyMemory<byte> InventoryDigest,
    string TargetOwnerId,
    long TargetOwnerLeaseGeneration,
    ReadOnlyMemory<byte> ApplicationPayload)
{
    internal bool IsCanonical { get; init; }
    internal uint TerminalCompletionCount { get; init; }
    internal uint PendingRelayCount { get; init; }
    internal long ApplicationVersion { get; init; }
    internal byte SourceCleanupState { get; init; }
}

internal static class ZLinkRelocationAuthorityPayloadCodec
{
    private const uint Magic = 0x5a4c4152; // ZLAR
    private const ushort Version = 1;

    internal static byte[] Encode(ZLinkRelocationAuthorityPayload payload)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        WriteString(writer, payload.Reference);
        writer.Write(payload.ChecksumCrc32c);
        writer.Write(payload.AggregateId.ToByteArray());
        writer.Write(payload.AggregateGeneration);
        WriteBytes(writer, payload.InventoryDigest.Span);
        WriteString(writer, payload.TargetOwnerId);
        writer.Write(payload.TargetOwnerLeaseGeneration);
        WriteBytes(writer, payload.ApplicationPayload.Span);
        writer.Flush();
        if (stream.Length > 1024 * 1024)
            throw new InvalidOperationException(
                "The authority relocation payload cannot exceed 1 MiB.");
        return stream.ToArray();
    }

    internal static bool TryDecode(
        ReadOnlySpan<byte> encoded,
        out ZLinkRelocationAuthorityPayload payload)
    {
        payload = null!;
        if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                encoded,
                out var canonical))
        {
            var aggregateGeneration = canonical.AggregateGeneration;
            if (aggregateGeneration == 0
                && ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
                    canonical.SteadyAuthorityPayload.Span,
                    out var actor)
                && actor.CurrentSpotKind == ZLinkSpotKind.Entry)
            {
                // A standalone Actor relocation always owns a one-participant
                // root at generation 1. Older canonical authority payloads did
                // not persist that value, but this case is unambiguous.
                aggregateGeneration = 1;
            }
            Span<byte> id = stackalloc byte[16];
            BinaryPrimitives.WriteUInt64BigEndian(id, canonical.RelocationHigh);
            BinaryPrimitives.WriteUInt64BigEndian(id[8..], canonical.RelocationLow);
            payload = new ZLinkRelocationAuthorityPayload(
                canonical.RelocationReference,
                canonical.RelocationChecksumCrc32c,
                new Guid(id, bigEndian: true),
                aggregateGeneration,
                new byte[32],
                canonical.TargetOwnerId,
                checked((long)canonical.TargetOwnerLeaseGeneration),
                canonical.SteadyAuthorityPayload)
            {
                IsCanonical = true,
                TerminalCompletionCount = canonical.TerminalCompletionCount,
                PendingRelayCount = canonical.PendingRelayCount,
                ApplicationVersion = canonical.ApplicationVersion,
                SourceCleanupState = canonical.SourceCleanupState
            };
            return true;
        }
        try
        {
            using var stream = new MemoryStream(encoded.ToArray(), writable: false);
            using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
            if (reader.ReadUInt32() != Magic || reader.ReadUInt16() != Version)
                return false;
            var reference = ReadString(reader);
            var checksum = reader.ReadUInt32();
            var aggregateId = new Guid(ReadExact(reader, 16));
            var aggregateGeneration = reader.ReadUInt64();
            var inventoryDigest = ReadBytes(reader);
            var targetOwnerId = ReadString(reader);
            var targetLeaseGeneration = reader.ReadInt64();
            var applicationPayload = ReadBytes(reader);
            if (aggregateId == Guid.Empty
                || aggregateGeneration is 0 or > long.MaxValue
                || inventoryDigest.Length != 32
                || targetLeaseGeneration <= 0
                || stream.Position != stream.Length)
                return false;
            payload = new ZLinkRelocationAuthorityPayload(
                reference,
                checksum,
                aggregateId,
                aggregateGeneration,
                inventoryDigest,
                targetOwnerId,
                targetLeaseGeneration,
                applicationPayload);
            return true;
        }
        catch (Exception error) when (error is IOException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        var encoded = Encoding.UTF8.GetBytes(value);
        if (encoded.Length is < 1 or > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(value));
        writer.Write((ushort)encoded.Length);
        writer.Write(encoded);
    }

    private static string ReadString(BinaryReader reader)
    {
        var size = reader.ReadUInt16();
        if (size == 0)
            throw new InvalidDataException();
        return Encoding.UTF8.GetString(ReadExact(reader, size));
    }

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        writer.Write(value.Length);
        writer.Write(value);
    }

    private static byte[] ReadBytes(BinaryReader reader)
    {
        var size = reader.ReadInt32();
        if (size < 0 || size > 1024 * 1024)
            throw new InvalidDataException();
        return ReadExact(reader, size);
    }

    private static byte[] ReadExact(BinaryReader reader, int size)
    {
        var value = reader.ReadBytes(size);
        if (value.Length != size)
            throw new EndOfStreamException();
        return value;
    }
}
