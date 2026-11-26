using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationCompletionProgress(
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
        ulong AcceptedRequests,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
        ulong CompletedResults,
    [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter))]
        ulong PendingDeliveries,
    string? Reference,
    string? Sha256
)
{
    internal static ZLinkRelocationCompletionProgress Start(ulong acceptedRequests) =>
        new(acceptedRequests, 0, 0, null, null);

    internal bool IsReady => AcceptedRequests == CompletedResults && PendingDeliveries == 0;

    internal static ZLinkRelocationCompletionProgress? Read(ZLinkAuthorityRelocation relocation) =>
        relocation.Completion?.Deserialize<ZLinkRelocationCompletionProgress>(
            ZLinkJsonSerializerOptions.Default
        );

    internal JsonElement ToJson() =>
        JsonSerializer.SerializeToElement(this, ZLinkJsonSerializerOptions.Default);
}

internal sealed class ZLinkRelocationCompletionStore(
    IZLinkLocationRepository authorities,
    IZLinkRelocationRepository blobs,
    ZLinkRelocationCompletionBudget? budget = null,
    TimeSpan? pollingInterval = null
)
{
    internal static string? CompletionBlocker(ZLinkAuthoritySnapshot source, ILogger? logger = null)
    {
        if (
            source.Relocation is not { } active
            || ZLinkRelocationCompletionProgress.Read(active) is not { IsReady: false } completion
        )
            return null;
        var reason =
            completion.PendingDeliveries > 0 ? "pending_delivery" : "completion_in_progress";
        logger?.LogWarning(
            "zlink.runtime.relocation.blocked reason={Reason} relocationId={RelocationId} pendingDeliveries={PendingDeliveries}",
            reason,
            active.RelocationId,
            completion.PendingDeliveries
        );
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_blocked reason={reason} relocation={active.RelocationId:N} pendingDeliveries={completion.PendingDeliveries}"
        );
        return $"Relocation '{active.RelocationId:N}' is completing with "
            + $"{completion.PendingDeliveries} pending deliveries and "
            + $"{completion.CompletedResults}/{completion.AcceptedRequests} stored results.";
    }

    internal static void RequireCompletionSettled(ZLinkAuthoritySnapshot source)
    {
        if (CompletionBlocker(source) is { } blocker)
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, blocker);
    }

    internal async ValueTask<ZLinkRelocationCompletionRecord> ReadAsync(
        ZLinkAuthorityRelocation relocation,
        CancellationToken cancellationToken,
        bool renew = false
    )
    {
        var progress =
            ZLinkRelocationCompletionProgress.Read(relocation)
            ?? throw DataLost("Completion authority is missing.");
        if (
            progress.CompletedResults > progress.AcceptedRequests
            || progress.PendingDeliveries > progress.CompletedResults
            || (progress.CompletedResults == 0) != (progress.Reference is null)
            || (progress.Reference is null) != (progress.Sha256 is null)
        )
            throw DataLost("Completion authority reference or counts disagree.");
        if (progress.Reference is null)
            return new(relocation.RelocationId, []);

        var record = await ZLinkRelocationTreeStore
            .ReadCompletionAsync(
                blobs,
                progress.Reference,
                progress.Sha256!,
                Convert.FromHexString(relocation.SourceInventorySha256),
                renew && progress.PendingDeliveries != 0,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (
            record.RelocationId != relocation.RelocationId
            || (ulong)record.Entries.Count != progress.CompletedResults
            || (ulong)record.PendingDeliveries != progress.PendingDeliveries
        )
            throw DataLost(
                "Relocation completion payload identity or counts differ from authority."
            );
        return record;
    }

    internal ValueTask<(
        ZLinkAuthoritySnapshot Authority,
        ZLinkRelocationCompletionEntry Entry
    )> AppendAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkRelocationCompletionEntry entry,
        CancellationToken cancellationToken
    ) =>
        UpdateAsync(
            key,
            expected,
            entry,
            ZLinkRelocationCompletionDelivery.Pending,
            cancellationToken
        );

    internal async ValueTask<ZLinkAuthoritySnapshot> AcknowledgeAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkRelocationCompletionEntry entry,
        ZLinkRelocationCompletionDelivery delivery,
        CancellationToken cancellationToken
    )
    {
        if (delivery == ZLinkRelocationCompletionDelivery.Pending)
            throw new ArgumentOutOfRangeException(nameof(delivery));
        var update = await UpdateAsync(key, expected, entry, delivery, cancellationToken)
            .ConfigureAwait(false);
        return update.Authority;
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> RedeliverPendingAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot current,
        Func<
            ZLinkAuthoritySnapshot,
            ZLinkRelocationCompletionEntry,
            CancellationToken,
            ValueTask<ZLinkRelocationCompletionDelivery>
        > deliver,
        CancellationToken cancellationToken
    )
    {
        var relocation = current.Relocation ?? throw DataLost("Completion authority is missing.");
        var record = await ReadAsync(relocation, cancellationToken, renew: true)
            .ConfigureAwait(false);
        if (record.Entries.Any(entry => entry.ObjectKey != key.Value))
            throw DataLost("Relocation completion references a different authority key.");
        foreach (
            var entry in record.Entries.Where(entry =>
                entry.Delivery == ZLinkRelocationCompletionDelivery.Pending
            )
        )
        {
            var delivery = await deliver(current, entry, cancellationToken).ConfigureAwait(false);
            if (delivery != ZLinkRelocationCompletionDelivery.Pending)
                current = await AcknowledgeAsync(key, current, entry, delivery, cancellationToken)
                    .ConfigureAwait(false);
        }
        return current;
    }

    internal static void RequireCompleted(ZLinkAuthorityRelocation relocation)
    {
        if (ZLinkRelocationCompletionProgress.Read(relocation) is not { IsReady: true })
            throw DataLost("Relocation still has incomplete requests or pending delivery.");
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> MarkCompletedAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot authority,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            var relocation =
                authority.Relocation ?? throw DataLost("Completion authority is missing.");
            RequireCompleted(relocation);
            if (
                relocation.Phase == ServiceWireCodec.RelocationPhase.Completed
                || ZLinkRelocationCompletionProgress.Read(relocation)!.AcceptedRequests == 0
            )
                return authority;

            try
            {
                var updated = await authorities
                    .CompareExchangeAuthorityAsync(
                        key,
                        authority.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            authority.Payload,
                            ZLinkAuthorityGenerationTransition.Preserve,
                            null,
                            null
                        )
                        {
                            Relocation = relocation with
                            {
                                Phase = ServiceWireCodec.RelocationPhase.Completed,
                            },
                        },
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (updated is ZLinkAuthorityCompareExchangeResult.Stored stored)
                    return stored.Snapshot;
                if (updated is not ZLinkAuthorityCompareExchangeResult.Conflict)
                    throw DataLost("Relocation completion phase CAS was rejected.");
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsIndeterminate(error, cancellationToken))
            {
                // The authority read below settles a possible applied CAS.
            }
            var observed = await ReadCurrentAsync(key, authority, cancellationToken)
                .ConfigureAwait(false);
            if (observed.StoreVersion == authority.StoreVersion)
                await WaitForStoreAsync(cancellationToken).ConfigureAwait(false);
            authority = observed;
        }
    }

    private async ValueTask<(
        ZLinkAuthoritySnapshot Authority,
        ZLinkRelocationCompletionEntry Entry
    )> UpdateAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkRelocationCompletionEntry entry,
        ZLinkRelocationCompletionDelivery delivery,
        CancellationToken cancellationToken
    )
    {
        if (entry.ObjectKey != key.Value)
            throw DataLost("Relocation completion references a different authority key.");
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                var result = await UpdateAtVersionAsync(
                        key,
                        expected,
                        entry,
                        delivery,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (result is not null)
                    return result.Value;

                expected = await ReadCurrentAsync(key, expected, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception error) when (IsStoreUnavailable(error, cancellationToken))
            {
                await RetainIfPendingAsync(expected.Relocation!, cancellationToken)
                    .ConfigureAwait(false);
                await WaitForStoreAsync(cancellationToken).ConfigureAwait(false);
            }
        }
    }

    internal async ValueTask<ZLinkAuthoritySnapshot> ReadCurrentAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        CancellationToken cancellationToken
    )
    {
        var fence = expected.Relocation ?? throw DataLost("Completion authority is missing.");
        ZLinkAuthorityReadResult read;
        try
        {
            read = await authorities
                .ReadAuthorityAsync(key, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (InvalidDataException)
        {
            throw DataLost("Relocation completion authority is malformed.");
        }
        if (
            read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.Relocation is not { } current
            || current.RelocationId != fence.RelocationId
            || current.Target != fence.Target
            || fence.Target is not { } target
            || !ZLinkAuthorityRelocation.MatchesCommittedTarget(
                found.Snapshot,
                fence.RelocationId,
                target.AttemptGeneration
            )
            || current.SourceInventorySha256 != fence.SourceInventorySha256
            || found.Snapshot.ObjectGeneration != expected.ObjectGeneration
            || found.Snapshot.AuthorityOwnerGeneration != expected.AuthorityOwnerGeneration
            || found.Snapshot.OwnerId != expected.OwnerId
            || found.Snapshot.OwnerLeaseGeneration != expected.OwnerLeaseGeneration
        )
            throw DataLost("Relocation completion authority changed its target fence.");
        return found.Snapshot;
    }

    private async ValueTask<(
        ZLinkAuthoritySnapshot Authority,
        ZLinkRelocationCompletionEntry Entry
    )?> UpdateAtVersionAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot expected,
        ZLinkRelocationCompletionEntry entry,
        ZLinkRelocationCompletionDelivery delivery,
        CancellationToken cancellationToken
    )
    {
        var relocation = expected.Relocation ?? throw DataLost("Completion authority is missing.");
        var record = await ReadAsync(relocation, cancellationToken).ConfigureAwait(false);
        var entries = record.Entries.ToList();
        var index = entries.FindIndex(entry.MatchesRequest);
        var progress = ZLinkRelocationCompletionProgress.Read(relocation)!;
        if (index >= 0)
        {
            var previous = entries[index];
            if (
                previous != (entry with { Delivery = previous.Delivery })
                && (
                    delivery != ZLinkRelocationCompletionDelivery.Pending
                    || budget?.GetFallback(entry) is not { } fallback
                    || previous != (fallback with { Delivery = previous.Delivery })
                )
            )
                throw DataLost("Relocation completion result differs from the stored request.");
            if (
                delivery == ZLinkRelocationCompletionDelivery.Pending
                || previous.Delivery != ZLinkRelocationCompletionDelivery.Pending
            )
                return (expected, previous);
            entry = previous with { Delivery = delivery };
            entries[index] = entry;
        }
        else
        {
            if (
                delivery != ZLinkRelocationCompletionDelivery.Pending
                || (ulong)entries.Count >= progress.AcceptedRequests
            )
                throw DataLost("Relocation completion has no matching accepted request.");
            if (budget is not null)
            {
                if (progress.AcceptedRequests != budget.AcceptedRequests)
                    throw DataLost(
                        "Relocation completion reservation differs from accepted requests."
                    );
                entry = budget.SelectReply(entries, entry);
            }
            var successor = entries.FindIndex(value =>
                ZLinkRelocationCompletionRecord.CompareObjectKeys(value.ObjectKey, entry.ObjectKey)
                > 0
            );
            entries.Insert(successor < 0 ? entries.Count : successor, entry);
        }

        record = record with { Entries = entries };
        var (reference, sha256) = await ZLinkRelocationTreeStore
            .PutCompletionAsync(
                blobs,
                record,
                Convert.FromHexString(relocation.SourceInventorySha256),
                cancellationToken
            )
            .ConfigureAwait(false);
        var replacement = progress with
        {
            Reference = reference,
            Sha256 = sha256,
            CompletedResults = (ulong)entries.Count,
            PendingDeliveries = (ulong)record.PendingDeliveries,
        };
        var mutation = new ZLinkAuthorityMutation.Put(
            expected.Payload,
            ZLinkAuthorityGenerationTransition.Preserve,
            null,
            null
        )
        {
            Relocation = relocation with { Completion = replacement.ToJson() },
        };
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                var exchanged = await authorities
                    .CompareExchangeAuthorityAsync(
                        key,
                        expected.StoreVersion,
                        mutation,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (exchanged is ZLinkAuthorityCompareExchangeResult.Stored stored)
                    return (stored.Snapshot, entry);
                if (exchanged is ZLinkAuthorityCompareExchangeResult.Conflict)
                    return null;
                throw DataLost("Relocation completion CAS was rejected.");
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsIndeterminate(error, cancellationToken))
            {
                ZLinkAuthoritySnapshot observed;
                while (true)
                {
                    try
                    {
                        observed = await ReadCurrentAsync(key, expected, cancellationToken)
                            .ConfigureAwait(false);
                        break;
                    }
                    catch (Exception readError)
                        when (IsStoreUnavailable(readError, cancellationToken))
                    {
                        await RetainIfPendingAsync(relocation, cancellationToken)
                            .ConfigureAwait(false);
                        await RetainIfPendingAsync(mutation.Relocation!, cancellationToken)
                            .ConfigureAwait(false);
                        await WaitForStoreAsync(cancellationToken).ConfigureAwait(false);
                    }
                }
                if (ZLinkRelocationCompletionProgress.Read(observed.Relocation!) == replacement)
                {
                    _ = await ReadAsync(observed.Relocation, cancellationToken)
                        .ConfigureAwait(false);
                    return (observed, entry);
                }
                if (observed.StoreVersion != expected.StoreVersion)
                    return null;
            }
        }
    }

    internal static bool IsStoreUnavailable(Exception error, CancellationToken token) =>
        !token.IsCancellationRequested
        && error is not ZLinkRelocationDataLostException
        && error
            is IOException
                or TimeoutException
                or ZLinkFrameworkException
                {
                    Kind: ZLinkFrameworkErrorKind.Unavailable
                        or ZLinkFrameworkErrorKind.Rejected
                        or ZLinkFrameworkErrorKind.DeadlineExceeded,
                };

    internal async ValueTask RetainIfPendingAsync(
        ZLinkAuthorityRelocation relocation,
        CancellationToken cancellationToken
    )
    {
        if (ZLinkRelocationCompletionProgress.Read(relocation) is not { PendingDeliveries: > 0 })
            return;
        try
        {
            _ = await ReadAsync(relocation, cancellationToken, renew: true).ConfigureAwait(false);
        }
        catch (Exception error)
            when (error is ZLinkRelocationDataLostException
                || IsStoreUnavailable(error, cancellationToken)
            )
        {
            // A reference from an uncertain or superseded CAS does not establish data loss.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_completion_renew_failed relocation={relocation.RelocationId:N} error={error.Message}"
            );
        }
    }

    private Task WaitForStoreAsync(CancellationToken cancellationToken) =>
        Task.Delay(pollingInterval ?? TimeSpan.FromSeconds(1), cancellationToken);

    private static ZLinkRelocationDataLostException DataLost(string message) => new(message);
}
