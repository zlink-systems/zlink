using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationCompletionStoreTests
{
    [Fact]
    [Trait("Contract", "RelocationCompletionPublication")]
    public async Task Publication_verifies_the_blob_before_one_cas_for_all_four_fields()
    {
        var calls = new List<string>();
        var blobs = new InMemoryRelocationStore
        {
            BeforeRepositoryPut = () => calls.Add("put"),
            OnRepositoryRead = () => calls.Add("read"),
        };
        var authority = new AuthorityStore { OnExchange = () => calls.Add("cas") };
        var writer = new ZLinkRelocationCompletionStore(authority, blobs);

        await AssertPublicationAsync(
            async () => await writer.AppendAsync(Key, authority.Snapshot, First, default),
            First,
            1
        );
        var firstReference = authority.Snapshot.Relocation!.Completion!.Reference!;
        var firstManifest = blobs.Payloads[firstReference].ToArray();
        await AssertPublicationAsync(
            async () =>
                await writer.AcknowledgeAsync(
                    Key,
                    authority.Snapshot,
                    First,
                    ZLinkRelocationCompletionDelivery.TerminalReceived,
                    default
                ),
            First with
            {
                Delivery = ZLinkRelocationCompletionDelivery.TerminalReceived,
            },
            0
        );
        Assert.Equal(firstManifest, blobs.Payloads[firstReference]);

        async Task AssertPublicationAsync(
            Func<Task> publish,
            ZLinkRelocationCompletionEntry expected,
            ulong pending
        )
        {
            var previous = authority.Snapshot.Relocation!.Completion!;
            var writes = authority.Writes;
            calls.Clear();
            await publish();

            Assert.Equal(
                new[] { "put", "read", "put", "read", "cas" },
                calls.SkipWhile(call => call == "read")
            );
            Assert.Equal(writes + 1, authority.Writes);

            var relocation = authority.Snapshot.Relocation!;
            var completion = relocation.Completion!;
            Assert.False(string.IsNullOrEmpty(completion.Reference));
            Assert.NotEqual(previous.Reference, completion.Reference);
            Assert.Matches("^[a-f0-9]{64}$", completion.Sha256!);
            Assert.NotEqual(previous.Sha256, completion.Sha256);
            Assert.Equal(2UL, completion.AcceptedRequests);
            Assert.Equal(1UL, completion.CompletedResults);
            Assert.Equal(pending, completion.PendingDeliveries);
            Assert.Equal(
                expected,
                Assert.Single((await writer.ReadAsync(relocation, default)).Entries)
            );
        }
    }

    [Fact]
    [Trait("Contract", "RelocationCompletionPublication")]
    public async Task Publication_persists_completed_only_after_both_accepted_results_are_delivered()
    {
        var authority = new AuthorityStore();
        var writer = new ZLinkRelocationCompletionStore(authority, new InMemoryRelocationStore());

        await writer.AppendAsync(Key, authority.Snapshot, First, default);
        await AssertNotCompletedAsync(1, 1);
        await writer.AcknowledgeAsync(
            Key,
            authority.Snapshot,
            First,
            ZLinkRelocationCompletionDelivery.TerminalReceived,
            default
        );
        await AssertNotCompletedAsync(1, 0);

        await writer.AppendAsync(Key, authority.Snapshot, Second, default);
        await AssertNotCompletedAsync(2, 1);
        await writer.AcknowledgeAsync(
            Key,
            authority.Snapshot,
            Second,
            ZLinkRelocationCompletionDelivery.TerminalReceived,
            default
        );
        var completion = AssertProgress(2, 0);
        var writes = authority.Writes;
        await writer.MarkCompletedAsync(Key, authority.Snapshot, default);

        Assert.Equal(
            ServiceWireCodec.RelocationPhase.Completed,
            authority.Snapshot.Relocation!.Phase
        );
        Assert.Equal(completion, authority.Snapshot.Relocation.Completion);
        Assert.Equal(writes + 1, authority.Writes);

        async Task AssertNotCompletedAsync(ulong completed, ulong pending)
        {
            AssertProgress(completed, pending);
            var before = authority.Writes;
            await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
                writer.MarkCompletedAsync(Key, authority.Snapshot, default).AsTask()
            );
            Assert.Equal(before, authority.Writes);
            Assert.Equal(
                ServiceWireCodec.RelocationPhase.Activated,
                authority.Snapshot.Relocation!.Phase
            );
        }

        ZLinkRelocationCompletionProgress AssertProgress(ulong completed, ulong pending)
        {
            var progress = authority.Snapshot.Relocation!.Completion!;
            Assert.Equal(
                (2UL, completed, pending),
                (progress.AcceptedRequests, progress.CompletedResults, progress.PendingDeliveries)
            );
            return progress;
        }
    }

    [Fact]
    [Trait("Contract", "RelocationCompletionPublication")]
    public async Task Publication_makes_no_location_cas_attempt_when_the_blob_put_fails()
    {
        var calls = new List<string>();
        var failure = new InvalidOperationException("Injected completion Put failure.");
        var blobs = new InMemoryRelocationStore
        {
            BeforeRepositoryPut = () =>
            {
                calls.Add("put");
                throw failure;
            },
        };
        var authority = new AuthorityStore { OnExchange = () => calls.Add("cas") };
        var initial = authority.Snapshot;
        var writer = new ZLinkRelocationCompletionStore(authority, blobs);

        var error = await Record.ExceptionAsync(() =>
            writer.AppendAsync(Key, initial, First, default).AsTask()
        );

        Assert.Equal(new[] { "put" }, calls);
        Assert.Same(failure, error);
        Assert.Same(initial, authority.Snapshot);
    }
}
