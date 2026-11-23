using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task ImmutableRootIsVerifiedBeforeAuthorityCasAndFreshTargetCannotRecover()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { ReplacementLeaseGeneration = 10 };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var request = CreateRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);
        using var services = new ServiceCollection().BuildServiceProvider();
        var freshTarget = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration()
        );
        var sourceNode = RoutingId.From("source");
        Assert.False(
            await freshTarget.PublishInboundAsync(
                new ZLinkAggregateFence(
                    request.Envelope.AggregateId,
                    request.Envelope.AggregateGeneration
                ),
                sourceNode,
                CancellationToken.None
            )
        );
        var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await authority.ReadAuthorityAsync(request.AuthorityKey)
        );
        Assert.Equal(published.Authority.OwnerId, current.Snapshot.OwnerId);
        Assert.Equal(
            published.Authority.OwnerLeaseGeneration,
            current.Snapshot.OwnerLeaseGeneration
        );
        Assert.Equal(request.Envelope.AggregateId, current.Snapshot.Relocation?.RelocationId);
        Assert.False(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(current.Snapshot.Payload.Span, out _)
        );

        var resolver = new ZLinkStoreLocationResolvers(
            authority,
            new ZLinkOwnerLeaseTracker(authority, new ZLinkLocationOptions())
        );
        var result = await resolver.ResolveActorRowWithStatusAsync(
            new ZLinkActorLocationKey("room")
        );
        Assert.Equal(ZLinkLocationResolutionKind.KnownUnavailable, result.Kind);
        Assert.Null(result.Row);

        var orderedEvents = relocation
            .Events.Concat(authority.Events)
            .OrderBy(static item => item.Sequence)
            .ToArray();
        var casSequence = Assert
            .Single(orderedEvents.Where(static item => item.Name == "cas"))
            .Sequence;
        Assert.All(
            orderedEvents.Where(static item => item.Name == "put"),
            item => Assert.True(item.Sequence < casSequence)
        );
        Assert.Contains(orderedEvents, item => item.Name == "get" && item.Sequence < casSequence);
        Assert.Equal("target-owner", published.Authority.OwnerId);
        Assert.Equal(9, published.Authority.OwnerLeaseGeneration);
        Assert.Equal(1UL, published.Authority.ObjectGeneration);
        Assert.Equal(1UL, published.Authority.AuthorityOwnerGeneration);
        Assert.Equal(
            request.Envelope.InventoryDigest.ToArray(),
            published.Envelope.InventoryDigest.ToArray()
        );
    }

    [Fact]
    public async Task CasConflictDeletesUnpublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { Conflict = true };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );

        await Assert.ThrowsAsync<ZLinkRelocationPublicationConflictException>(async () =>
            await coordinator.PublishAsync(CreateRequest(CreateEnvelope()))
        );

        Assert.DoesNotContain(
            relocation.Payloads.Values,
            static payload => payload.Take(4).SequenceEqual("ZLTM"u8.ToArray())
        );
        Assert.Contains(relocation.Events, static item => item.Name == "delete");
    }

    [Fact]
    public async Task UnpublishedOrphanCleanupFailurePreservesConflictAndReportsCause()
    {
        var expected = new IOException("orphan cleanup failed");
        var failures = new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter();
        var relocation = new RecordingRelocationStore { DeleteFailure = expected };
        var authority = new RecordingAuthorityStore { Conflict = true };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            failures,
            authority,
            relocation
        );

        await Assert.ThrowsAsync<ZLinkRelocationPublicationConflictException>(async () =>
            await coordinator.PublishAsync(CreateRequest(CreateEnvelope()))
        );

        Assert.Contains(expected, failures.Failures);
        Assert.Contains(relocation.Events, static item => item.Name == "delete");
        Assert.NotEmpty(relocation.Payloads);
    }

    [Fact]
    public async Task ExceptionAfterCommittedCasReconcilesWithoutDeletingPublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { ThrowAfterCommit = true };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );

        var published = await coordinator.PublishAsync(CreateRequest(CreateEnvelope()));

        Assert.Equal(published.Envelope.AggregateId, published.Authority.Relocation?.RelocationId);
        Assert.Equal(new byte[] { 8, 8 }, published.Authority.Payload.ToArray());
        var cas = Assert.Single(authority.Events.Where(static item => item.Name == "cas"));
        Assert.Contains(
            authority.Events,
            item => item.Name == "read" && item.Sequence > cas.Sequence
        );
        Assert.DoesNotContain(
            relocation.Events,
            item => item.Name == "get" && item.Sequence > cas.Sequence
        );
        Assert.NotEmpty(relocation.Payloads);
        Assert.DoesNotContain(relocation.Events, static item => item.Name == "delete");
    }

    [Fact]
    public async Task IndeterminateAuthorityReadAfterCommittedCasPreservesPublishedRootAndFailures()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            ThrowAfterCommit = true,
            ReadFailuresRemaining = 1,
        };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );

        var failure = await Assert.ThrowsAsync<AggregateException>(async () =>
            await coordinator.PublishAsync(CreateRequest(CreateEnvelope()))
        );

        Assert.Collection(
            failure.InnerExceptions,
            error => Assert.Equal("commit outcome unknown", error.Message),
            error => Assert.Equal("The authority read result is indeterminate.", error.Message)
        );
        Assert.NotEmpty(relocation.Payloads);
        Assert.DoesNotContain(relocation.Events, static item => item.Name == "delete");
    }

    [Fact]
    public async Task PreparedRootCanBeReadByManifestBeforeAuthorityPublication()
    {
        var authority = new RecordingAuthorityStore();
        var relocation = new RecordingRelocationStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var envelope = CreateEnvelope();

        var prepared = await coordinator.PrepareAsync(envelope);
        var restored = await coordinator.ReadPreparedAsync(prepared.Reference);

        Assert.Empty(authority.Events);
        Assert.Equal(envelope.AggregateId, restored.AggregateId);
        Assert.Equal(
            envelope.Participants.Select(static item => item.AuthorityKey),
            restored.Participants.Select(static item => item.AuthorityKey)
        );
    }

    [Fact]
    public async Task ExactPublishedConflictReconcilesWithoutDeletingRoot()
    {
        var authority = new RecordingAuthorityStore();
        var relocation = new RecordingRelocationStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var request = CreateRequest(CreateEnvelope());
        var prepared = await coordinator.PrepareAsync(request.Envelope);
        var previous = await coordinator.PublishPreparedAsync(request, prepared);
        authority.Conflict = true;

        var published = await coordinator.PublishPreparedAsync(request, prepared);

        Assert.Equal(previous.Relocation.Reference, published.Relocation.Reference);
        Assert.True(relocation.Payloads.ContainsKey(previous.Relocation.Reference));
        Assert.Equal(2, authority.Events.Count(static item => item.Name == "cas"));
    }
}
