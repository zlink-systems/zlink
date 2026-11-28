using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RelocationCompletionStoreTests
{
    [Fact]
    public async Task RowRejectsUnknownFormatAndIdentityMismatch()
    {
        var (_, _, _, row) = await SeedAsync();
        var bytes = row.Encode();
        Assert.Equal(row, ZLinkRelocationReplyRow.Decode(row.Key, bytes));
        var unknownVersion = System.Text.Encoding.UTF8.GetBytes(
            System
                .Text.Encoding.UTF8.GetString(bytes)
                .Replace(
                    "\"formatVersion\":\"1\"",
                    "\"formatVersion\":\"2\"",
                    StringComparison.Ordinal
                )
        );
        Assert.Throws<ZLinkRelocationDataLostException>(() =>
            ZLinkRelocationReplyRow.Decode(row.Key, unknownVersion)
        );
        var other = row with { RelocationId = Guid.NewGuid() };
        Assert.Throws<ZLinkRelocationDataLostException>(() =>
            ZLinkRelocationReplyRow.Decode(other.Key, bytes)
        );
    }

    [Fact]
    public async Task LostResultResponseReconcilesTheIdenticalRowAndKeepsTheFirstReply()
    {
        var (provider, repository, store, row) = await SeedAsync();
        provider.LoseResponseFor = row.Key;
        var pending = await store.PublishAsync(row, static () => "BAUG", default);
        Assert.Equal(ZLinkRelocationCompletionDelivery.Pending, pending!.Entry.Delivery);
        Assert.Equal(1, provider.LostResponses);
        Assert.Equal(1, provider.ResultWrites);

        var duplicate = row with { Entry = row.Entry with { Reply = "BwgJ" } };
        Assert.Equal(pending, await store.PublishAsync(duplicate, static () => "BAUG", default));
        Assert.Equal("AQID", pending.Entry.Reply);
        Assert.Equal(1, provider.ResultWrites);
        Assert.Equal(
            pending,
            Assert.Single(await repository.ReadRelocationRepliesAsync(row.RelocationId, default))
        );
    }

    [Theory]
    [InlineData("TerminalReceived")]
    [InlineData("AlreadyTerminal")]
    [InlineData("SourceLeaseExpired")]
    public async Task TerminalEvidenceIsImmutableAndExpiresAfterFiveMinutes(string evidenceName)
    {
        var clock = new ControllableTimeProvider();
        var (provider, repository, store, row) = await SeedAsync(clock);
        var evidence = Enum.Parse<ZLinkRelocationCompletionDelivery>(evidenceName);
        var pending = (await store.PublishAsync(row, static () => "BAUG", default))!;
        var terminal = await store.AcknowledgeAsync(pending, evidence, default);
        Assert.Equal(evidence, terminal!.Entry.Delivery);
        Assert.Equal("AQID", terminal.Entry.Reply);

        clock.Advance(TimeSpan.FromMinutes(4));
        Assert.Equal(terminal, await store.AcknowledgeAsync(pending, evidence, default));
        var competing =
            evidence == ZLinkRelocationCompletionDelivery.TerminalReceived
                ? ZLinkRelocationCompletionDelivery.AlreadyTerminal
                : ZLinkRelocationCompletionDelivery.TerminalReceived;
        Assert.Equal(terminal, await store.AcknowledgeAsync(pending, competing, default));
        Assert.Equal(2, provider.ResultWrites);
        clock.Advance(TimeSpan.FromMinutes(1));
        Assert.Empty(await repository.ReadRelocationRepliesAsync(row.RelocationId, default));
    }

    [Fact]
    public async Task AReplacementWriterCannotChangeAnOldRowAndRenewalUsesStoreTime()
    {
        var clock = new ControllableTimeProvider();
        var (provider, repository, store, row) = await SeedAsync(clock);
        var pending = (await store.PublishAsync(row, static () => "BAUG", default))!;
        clock.Advance(TimeSpan.FromHours(12));
        Assert.Equal(pending, await store.RenewAsync(row, default));
        Assert.Equal(2, provider.ResultWrites);
        clock.Advance(TimeSpan.FromHours(12));
        Assert.Equal(
            pending,
            Assert.Single(await repository.ReadRelocationRepliesAsync(row.RelocationId, default))
        );
        clock.Advance(TimeSpan.FromHours(1));
        var replacement = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync(row.Writer.OwnerId, TimeSpan.FromHours(1))
            )
            .Token;
        Assert.NotEqual(row.Writer, replacement);
        Assert.Null(
            await store.AcknowledgeAsync(
                pending,
                ZLinkRelocationCompletionDelivery.TerminalReceived,
                default
            )
        );
        Assert.Equal(2, provider.ResultWrites);
    }

    [Fact]
    public async Task AlreadyTerminalAckFromAnotherSourceFenceLeavesTheReplyRowPending()
    {
        await using var host = await RelocationBehaviorHost.StartAsync(
            "reply-row-ack",
            new RelocationBehaviorTrace(),
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            false
        );
        var node = host.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName);
        var current = node.LocalRequestSource;
        var rid = current.NodeRid;
        var expected = current with { OwnerId = "previous-request-owner", NodeGeneration = 1 };
        Assert.NotEqual(expected.NodeGeneration, current.NodeGeneration);
        var (_, repository, store, row) = await SeedAsync(
            requestSource: new(
                expected.OwnerId,
                expected.LeaseGeneration,
                rid.ToHex(),
                expected.NodeGeneration
            )
        );
        var pending = await store.PublishAsync(row, static () => "BAUG", default);
        Assert.NotNull(pending);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(1, 2),
            row.Entry.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(1, 1),
            current.NodeGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                current.OwnerId,
                current.LeaseGeneration,
                rid,
                current.NodeGeneration,
                "reply-row-root"
            ),
            1,
            1,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None
        );

        var ack = await node.RelayRelocationReplyAsync(
            rid,
            relay,
            current,
            [],
            TimeSpan.FromSeconds(1),
            default
        );
        Assert.Equal((byte)ZLinkRelocationReplyCompletionState.AlreadyTerminal, ack.Status);
        Assert.Equal(current, ack.RequestSource);
        await Assert.ThrowsAsync<TimeoutException>(() =>
            node.RelayRelocationReplyAsync(
                    rid,
                    relay,
                    expected,
                    [],
                    TimeSpan.FromMilliseconds(50),
                    default
                )
                .AsTask()
        );
        Assert.Equal(
            pending,
            Assert.Single(await repository.ReadRelocationRepliesAsync(row.RelocationId, default))
        );
        Assert.Equal(ZLinkRelocationCompletionDelivery.Pending, pending.Entry.Delivery);
    }

    [Fact]
    public async Task SourceLeaseEvidenceUsesTheOriginalLeaseGenerationAndStoreTime()
    {
        var clock = new ControllableTimeProvider();
        var (_, repository, store, row) = await SeedAsync(clock);
        var pending = (await store.PublishAsync(row, static () => "BAUG", default))!;
        var source = row.Entry.Source;
        Assert.False(
            ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
                await repository.ReadOwnerLeaseAsync(source.OwnerId, default),
                source.OwnerId,
                source.OwnerLeaseGeneration
            )
        );

        clock.Advance(TimeSpan.FromMinutes(1));
        Assert.True(
            ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
                await repository.ReadOwnerLeaseAsync(source.OwnerId, default),
                source.OwnerId,
                source.OwnerLeaseGeneration
            )
        );
        var terminal = await store.AcknowledgeAsync(
            pending,
            ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
            default
        );
        Assert.Equal(
            ZLinkRelocationCompletionDelivery.SourceLeaseExpired,
            terminal!.Entry.Delivery
        );
    }

    [Fact]
    public async Task AcceptedRenewalPreservesTheStateAndAResultCanArriveAfterTheOriginalExpiry()
    {
        var clock = new ControllableTimeProvider();
        var (_, repository, store, row) = await SeedAsync(clock);
        clock.Advance(TimeSpan.FromHours(12));
        await repository.RenewAcceptedRelocationRepliesAsync(row.RelocationId, row.Writer, default);
        clock.Advance(TimeSpan.FromHours(12));
        var accepted = Assert.Single(
            await repository.ReadRelocationRepliesAsync(row.RelocationId, default)
        );
        Assert.Equal(ZLinkRelocationCompletionDelivery.Accepted, accepted.Entry.Delivery);
        Assert.Equal(
            ZLinkRelocationCompletionDelivery.Pending,
            (await store.PublishAsync(row, static () => "BAUG", default))!.Entry.Delivery
        );
    }

    [Fact]
    public async Task OversizedResultUsesTheFailureReply()
    {
        var (_, _, store, row) = await SeedAsync();
        var oversized = row with
        {
            Entry = row.Entry with
            {
                Reply = new string('a', ZLinkRelocationReplyRow.MaximumBytes),
            },
        };
        var pending = await store.PublishAsync(oversized, static () => "BAUG", default);
        Assert.Equal("BAUG", pending!.Entry.Reply);
        Assert.Equal(ZLinkRelocationCompletionDelivery.Pending, pending.Entry.Delivery);
    }

    private static async Task<(
        LossyProvider Provider,
        ZLinkProviderLocationRepository Repository,
        ZLinkRelocationCompletionStore Store,
        ZLinkRelocationReplyRow Row
    )> SeedAsync(TimeProvider? clock = null, ZLinkCanonicalRequestSourceFence? requestSource = null)
    {
        var provider = new LossyProvider(new ZLinkInMemoryProviderLocationStore(clock));
        var repository = new ZLinkProviderLocationRepository(provider);
        var writer = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync("target", TimeSpan.FromHours(25))
            )
            .Token;
        var source = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync("source", TimeSpan.FromMinutes(1))
            )
            .Token;
        var row = new ZLinkRelocationReplyRow(
            Guid.NewGuid(),
            writer,
            new(
                "zla1:a:actor",
                "00000000000000010000000000000002",
                5,
                "AQID",
                requestSource ?? new(source.OwnerId, (ulong)source.LeaseGeneration, "01", 12)
            )
        );
        var accepted = row with
        {
            Entry = row.Entry with
            {
                Delivery = ZLinkRelocationCompletionDelivery.Accepted,
                Reply = "",
            },
        };
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new(
                    [new ZLinkStoreCondition.Missing(row.Key)],
                    [
                        new ZLinkStoreMutation.Put(
                            row.Key,
                            accepted.Encode(),
                            ZLinkRelocationReplyRow.ActiveRetention
                        ),
                    ]
                )
            )
        );
        return (provider, repository, new(repository), row);
    }

    private sealed class LossyProvider(IZLinkLocationStore inner) : IZLinkLocationStore
    {
        internal ZLinkStoreKey? LoseResponseFor { get; set; }
        internal int LostResponses { get; private set; }
        internal int ResultWrites { get; private set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var result = await inner.WriteAsync(request, cancellationToken);
            if (result is ZLinkStoreWriteResult.Applied)
            {
                foreach (var version in request.Conditions.OfType<ZLinkStoreCondition.Version>())
                {
                    if (
                        !version.Key.Value.StartsWith(
                            "$zlink/relocation-reply/",
                            StringComparison.Ordinal
                        )
                    )
                        continue;
                    ResultWrites++;
                    if (LoseResponseFor == version.Key)
                    {
                        LostResponses++;
                        LoseResponseFor = null;
                        throw new IOException("Lost the Location Store write response.");
                    }
                }
            }
            return result;
        }
    }
}
