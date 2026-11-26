using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RelocationCompletionStoreTests
{
    private static readonly ZLinkAuthorityKey Key = new("zla1:a:actor");
    private static readonly ZLinkRelocationCompletionEntry First = new(
        Key.Value,
        "00000000000000010000000000000002",
        5,
        "AQID",
        new("source", 7, "01", 12)
    );
    private static readonly ZLinkRelocationCompletionEntry Second = First with
    {
        OperationId = "00000000000000010000000000000003",
        ReplyRouteId = 6,
        Reply = "BAUG",
    };

    [Fact]
    public async Task Publishes_verified_result_before_one_cas_and_redelivers_only_pending_after_reopen()
    {
        var blobs = new InMemoryRelocationStore();
        var authority = new AuthorityStore();
        var writer = new ZLinkRelocationCompletionStore(authority, blobs);
        authority.BeforeExchange = async relocation =>
        {
            var record = await writer.ReadAsync(relocation, default);
            Assert.Equal(relocation.RelocationId, record.RelocationId);
            Assert.NotEmpty(record.Entries);
        };

        var initial = authority.Snapshot;
        Assert.Throws<ZLinkRelocationDataLostException>(() =>
            ZLinkRelocationCompletionStore.RequireCompleted(initial.Relocation!)
        );
        var (published, _) = await writer.AppendAsync(Key, initial, First, default);
        Assert.Equal(1, authority.Writes);
        var acknowledged = await writer.AcknowledgeAsync(
            Key,
            published,
            First,
            ZLinkRelocationCompletionDelivery.TerminalReceived,
            default
        );
        var (pending, _) = await writer.AppendAsync(Key, acknowledged, Second, default);
        Assert.Throws<ZLinkRelocationDataLostException>(() =>
            ZLinkRelocationCompletionStore.RequireCompleted(pending.Relocation!)
        );
        Assert.NotEqual(
            ZLinkRelocationCompletionProgress.Read(published.Relocation!)!.Reference,
            ZLinkRelocationCompletionProgress.Read(pending.Relocation!)!.Reference
        );

        var reader = new ZLinkRelocationCompletionStore(authority, blobs);
        var delivered = new List<ZLinkRelocationCompletionEntry>();
        var final = await reader.RedeliverPendingAsync(
            Key,
            authority.Snapshot,
            (_, entry, _) =>
            {
                delivered.Add(entry);
                return ValueTask.FromResult(ZLinkRelocationCompletionDelivery.AlreadyTerminal);
            },
            default
        );
        Assert.Equal(Second, Assert.Single(delivered));
        Assert.Equal(4, authority.Writes);
        ZLinkRelocationCompletionStore.RequireCompleted(final.Relocation!);
        var stored = await reader.ReadAsync(final.Relocation!, default);
        Assert.Equal(
            ZLinkRelocationCompletionDelivery.TerminalReceived,
            stored.Entries[0].Delivery
        );
        Assert.Equal(ZLinkRelocationCompletionDelivery.AlreadyTerminal, stored.Entries[1].Delivery);
    }

    [Fact]
    public async Task Acknowledgement_with_an_older_version_preserves_a_concurrent_append()
    {
        var authority = new AuthorityStore();
        var store = new ZLinkRelocationCompletionStore(authority, new InMemoryRelocationStore());
        var (first, _) = await store.AppendAsync(Key, authority.Snapshot, First, default);
        await store.AppendAsync(Key, first, Second, default);

        var acknowledged = await store.AcknowledgeAsync(
            Key,
            first,
            First,
            ZLinkRelocationCompletionDelivery.TerminalReceived,
            default
        );

        Assert.Equal(1, authority.Conflicts);
        Assert.Equal(3, authority.Writes);
        var record = await store.ReadAsync(acknowledged.Relocation!, default);
        Assert.Equal(
            new[]
            {
                First with
                {
                    Delivery = ZLinkRelocationCompletionDelivery.TerminalReceived,
                },
                Second,
            },
            record.Entries
        );
    }

    [Fact]
    public async Task Uncertain_cas_reuses_the_verified_manifest_when_authority_is_unchanged()
    {
        var authority = new AuthorityStore();
        var blobs = new InMemoryRelocationStore();
        var store = new ZLinkRelocationCompletionStore(authority, blobs);
        var expected = authority.Snapshot;
        var attempts = new List<string>();
        authority.BeforeExchange = relocation =>
        {
            attempts.Add(ZLinkRelocationCompletionProgress.Read(relocation)!.Reference!);
            if (attempts.Count == 1)
                throw new TimeoutException("Location Store did not return the CAS result.");
            return ValueTask.CompletedTask;
        };

        await store.AppendAsync(Key, expected, First, default);

        Assert.Equal(2, attempts.Count);
        Assert.Equal(attempts[0], attempts[1]);
        Assert.Equal(2, blobs.Payloads.Count);
        Assert.Equal(1, authority.Writes);
        Assert.Equal(
            First,
            Assert.Single((await store.ReadAsync(authority.Snapshot.Relocation!, default)).Entries)
        );
    }

    [Fact]
    public async Task Pending_completion_renews_manifest_and_chunks_without_acknowledging()
    {
        var authority = new AuthorityStore();
        var blobs = new InMemoryRelocationStore();
        var store = new ZLinkRelocationCompletionStore(
            authority,
            new ZLinkProviderRelocationRepository(blobs)
        );
        var (published, _) = await store.AppendAsync(Key, authority.Snapshot, First, default);
        var components = blobs.Payloads.ToArray();
        Assert.Equal(2, components.Length);
        foreach (var (reference, payload) in components)
            Assert.IsType<ZLinkBlobPutResult.AlreadyStored>(
                await blobs.PutAsync(new(reference), payload, TimeSpan.FromHours(11))
            );

        var after = await store.RedeliverPendingAsync(
            Key,
            published,
            (_, _, _) => ValueTask.FromResult(ZLinkRelocationCompletionDelivery.Pending),
            default
        );

        Assert.Equal(published, after);
        Assert.Equal(1, authority.Writes);
        foreach (var (reference, _) in components)
        {
            var renewed = Assert.IsType<ZLinkBlobReadResult.Found>(
                await blobs.ReadAsync(new(reference))
            );
            Assert.True(renewed.ExpiresAt - renewed.StoreNow > TimeSpan.FromHours(23));
        }
    }

    [Fact]
    public async Task Stored_reply_requires_the_exact_committed_target_attempt()
    {
        var authority = new AuthorityStore();
        var store = new ZLinkRelocationCompletionStore(authority, new InMemoryRelocationStore());
        var snapshot = authority.Snapshot;
        Assert.Equal(snapshot, await store.ReadCurrentAsync(Key, snapshot, default));
        var relocation = snapshot.Relocation!;
        var stale = snapshot with
        {
            Relocation = relocation with
            {
                Target = relocation.Target! with { AttemptGeneration = 2 },
            },
        };

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            store.ReadCurrentAsync(Key, stale, default).AsTask()
        );
    }

    [Fact]
    public async Task Reply_is_published_once_when_applied_cas_response_and_first_read_are_lost()
    {
        var authority = new AuthorityStore { LoseAfterExchange = true };
        var blobs = new InMemoryRelocationStore();
        var store = new ZLinkRelocationCompletionStore(
            authority,
            blobs,
            pollingInterval: TimeSpan.FromMilliseconds(1)
        );
        var expected = authority.Snapshot;

        var (current, saved) = await store.AppendAsync(Key, expected, First, default);

        Assert.Equal(1, authority.Writes);
        Assert.Equal(2, authority.AuthorityReads);
        Assert.Equal(authority.Snapshot, current);
        Assert.Equal(First, saved);
        Assert.Equal(2, blobs.Payloads.Count);
    }

    [Fact]
    public async Task Multi_chunk_completion_round_trips_and_rejects_a_missing_later_chunk()
    {
        var authority = new AuthorityStore();
        var blobs = new InMemoryRelocationStore();
        var store = new ZLinkRelocationCompletionStore(authority, blobs);
        var reply = new string('A', ZLinkRelocationTreeStore.ChunkBytes / 2);
        var (first, _) = await store.AppendAsync(
            Key,
            authority.Snapshot,
            First with
            {
                Reply = reply,
            },
            default
        );
        var existing = blobs.Payloads.Keys.ToHashSet();
        var (current, _) = await store.AppendAsync(
            Key,
            first,
            Second with
            {
                Reply = reply,
            },
            default
        );
        var relocation = current.Relocation!;
        var reference = ZLinkRelocationCompletionProgress.Read(relocation)!.Reference!;
        var newChunks = blobs
            .Payloads.Where(pair => !existing.Contains(pair.Key) && pair.Key != reference)
            .OrderBy(pair => pair.Value.Length)
            .ToArray();

        Assert.Equal(2, newChunks.Length);
        var record = await store.ReadAsync(relocation, default);
        Assert.Equal(
            new[] { First.OperationId, Second.OperationId },
            record.Entries.Select(entry => entry.OperationId)
        );
        Assert.All(record.Entries, entry => Assert.Equal(reply, entry.Reply));

        blobs.Remove(newChunks[0].Key);
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            store.ReadAsync(relocation, default).AsTask()
        );
    }

    [Fact]
    public async Task Reserves_unfinished_replies_and_publishes_the_fallback_before_delivery()
    {
        var authority = new AuthorityStore();
        var relocationId = authority.Snapshot.Relocation!.RelocationId;
        var fallback = First with { Reply = "AAAA" };
        var bound = (ulong)
            new ZLinkRelocationCompletionRecord(
                relocationId,
                new[] { fallback, Second }
                    .Select(entry =>
                        entry with
                        {
                            Delivery = ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
                        }
                    )
                    .ToArray()
            )
                .Encode()
                .Length;
        var rejected = new ZLinkRelocationCompletionBudget(
            relocationId,
            [fallback, Second],
            bound - 1
        );
        var error = Assert.Throws<ZLinkFrameworkException>(rejected.EnsureFitsBeforeCommit);
        Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, error.Kind);
        var budget = new ZLinkRelocationCompletionBudget(
            relocationId,
            [fallback, Second],
            bound + 1
        );
        budget.EnsureFitsBeforeCommit();
        var store = new ZLinkRelocationCompletionStore(
            authority,
            new InMemoryRelocationStore(),
            budget
        );

        var large = First with { Reply = "AQIDBAUG" };
        var (published, selected) = await store.AppendAsync(
            Key,
            authority.Snapshot,
            large,
            default
        );
        Assert.Equal(fallback, selected);
        Assert.Equal(
            fallback,
            Assert.Single((await store.ReadAsync(published.Relocation!, default)).Entries)
        );
        var acknowledged = await store.AcknowledgeAsync(
            Key,
            published,
            selected,
            ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
            default
        );
        var (retried, saved) = await store.AppendAsync(Key, acknowledged, large, default);
        Assert.Equal(acknowledged, retried);
        Assert.Equal(ZLinkRelocationCompletionDelivery.SourceLeaseExpired, saved.Delivery);
        var (finalPending, second) = await store.AppendAsync(Key, retried, Second, default);
        var final = await store.AcknowledgeAsync(
            Key,
            finalPending,
            second,
            ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
            default
        );
        ZLinkRelocationCompletionStore.RequireCompleted(final.Relocation!);
        Assert.Equal(4, authority.Writes);
    }

    [Theory]
    [InlineData("missing")]
    [InlineData("manifest")]
    [InlineData("chunk")]
    [InlineData("sha256")]
    [InlineData("relocationId")]
    [InlineData("completedResults")]
    [InlineData("pendingDeliveries")]
    public async Task Read_fails_closed_when_completion_and_authority_disagree(string damage)
    {
        var blobs = new InMemoryRelocationStore();
        var authority = new AuthorityStore();
        var writer = new ZLinkRelocationCompletionStore(authority, blobs);
        var (published, _) = await writer.AppendAsync(Key, authority.Snapshot, First, default);
        var relocation = published.Relocation!;
        var completion = ZLinkRelocationCompletionProgress.Read(relocation)!;
        var reference = completion.Reference!;

        switch (damage)
        {
            case "missing":
                blobs.Remove(reference);
                break;
            case "manifest":
                blobs.Payloads[reference][0] ^= 1;
                break;
            case "chunk":
                blobs.Payloads.Single(pair => pair.Key != reference).Value[0] ^= 1;
                break;
            case "sha256":
                relocation = relocation with
                {
                    Completion = (completion with { Sha256 = new string('0', 64) }).ToJson(),
                };
                break;
            case "relocationId":
                relocation = relocation with { RelocationId = Guid.NewGuid() };
                break;
            case "completedResults":
                relocation = relocation with
                {
                    Completion = (completion with { CompletedResults = 2 }).ToJson(),
                };
                break;
            case "pendingDeliveries":
                relocation = relocation with
                {
                    Completion = (completion with { PendingDeliveries = 0 }).ToJson(),
                };
                break;
        }

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            writer.ReadAsync(relocation, default).AsTask()
        );
    }

    private sealed class AuthorityStore : ZLinkLocationStoreTestDouble
    {
        private static readonly ZLinkAuthorityRelocation.Fence Source = new("01", 12, "source", 7);

        internal ZLinkAuthoritySnapshot Snapshot { get; private set; } =
            new(
                "initial",
                new byte[] { 42 },
                1,
                2,
                "target",
                3,
                new(
                    ZLinkPlacementAllocationState.Active,
                    ZLinkPlacementObjectKind.Actor,
                    "Test.Actor",
                    new("mesh", RoutingId.FromHex("02")),
                    13,
                    new(1, 0, null)
                ),
                null,
                DateTimeOffset.UtcNow
            )
            {
                Relocation = new(
                    Guid.Parse("0f1e2d3c-4b5a-4968-8778-a1b2c3d4e5f6"),
                    ServiceWireCodec.RelocationPhase.Activated,
                    Source,
                    new("02", 13, "target", 3, 1),
                    new("01", 12, "source", 7, "original"),
                    new string('a', 64),
                    1,
                    ZLinkRelocationCompletionProgress.Start(2).ToJson()
                ),
            };

        internal int Writes { get; private set; }
        internal int AuthorityReads { get; private set; }
        internal bool LoseAfterExchange { get; init; }
        internal int Conflicts { get; private set; }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        )
        {
            Assert.Equal(Key, key);
            AuthorityReads++;
            if (LoseAfterExchange && AuthorityReads == 1)
                throw new TimeoutException("Location Store did not return the authority read.");
            return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                new ZLinkAuthorityReadResult.Found(Snapshot)
            );
        }

        internal Func<ZLinkAuthorityRelocation, ValueTask>? BeforeExchange { get; set; }

        public override async ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        )
        {
            Assert.Equal(Key, key);
            if (Snapshot.StoreVersion != expectedStoreVersion)
            {
                Conflicts++;
                return new ZLinkAuthorityCompareExchangeResult.Conflict(
                    new ZLinkAuthorityReadResult.Found(Snapshot)
                );
            }
            var put = Assert.IsType<ZLinkAuthorityMutation.Put>(mutation);
            Assert.Equal(Snapshot.Payload, put.Payload);
            Assert.Equal(ZLinkAuthorityGenerationTransition.Preserve, put.GenerationTransition);
            var relocation = Assert.IsType<ZLinkAuthorityRelocation>(put.Relocation);
            if (BeforeExchange is { } check)
                await check(relocation);
            Writes++;
            Snapshot = Snapshot with
            {
                StoreVersion = $"version-{Writes}",
                Relocation = relocation,
            };
            if (LoseAfterExchange)
                throw new TimeoutException("Location Store applied the CAS but lost its response.");
            return new ZLinkAuthorityCompareExchangeResult.Stored(Snapshot);
        }
    }
}
