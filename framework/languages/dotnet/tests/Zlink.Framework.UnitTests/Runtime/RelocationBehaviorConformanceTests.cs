using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RelocationBehaviorConformanceTests
{
    [Fact]
    public void Target_attempt_slot_keeps_identity_until_last_user_quiesces()
    {
        var slots = new ConcurrentDictionary<string, ZLinkRelocationAttemptLeaseState>();
        var slot = slots.GetOrAdd(
            "attempt",
            static _ => new ZLinkRelocationAttemptLeaseState());
        Assert.True(slot.TryAcquire());
        Assert.True(slot.TryAcquire());

        slot.MarkClosing();
        Assert.False(slot.TryAcquire());
        Assert.False(slot.CanRemove);
        Assert.False(slot.Release());
        Assert.False(slot.CanRemove);
        Assert.Same(slot, slots["attempt"]);

        Assert.True(slot.Release());
        Assert.True(slot.CanRemove);
        Assert.True(slots.TryRemove(
            new KeyValuePair<string, ZLinkRelocationAttemptLeaseState>(
                "attempt",
                slot)));
        var replacement = slots.GetOrAdd(
            "attempt",
            static _ => new ZLinkRelocationAttemptLeaseState());
        Assert.NotSame(slot, replacement);
        Assert.True(slot.Quiesced.IsCompletedSuccessfully);
    }

    [Fact]
    public async Task ActorJoin_overlapping_prepare_timeout_keeps_original_waiter_owned()
    {
        var trace = new RelocationBehaviorTrace();
        var transport = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: true);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-overlap-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-overlap-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await transport.PrepareCallStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            transport.ReleasePrepareCall.TrySetResult();
            await transport.TargetPreparedBeforeReadySend.Task.WaitAsync(
                TimeSpan.FromSeconds(3));

            var overlapping = transport.RetryPrepareAsync(
                    CancellationToken.None,
                    TimeSpan.FromMilliseconds(250))
                .AsTask();
            var timeout = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await overlapping);
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timeout.Kind);

            transport.ReleaseTargetReadySend.TrySetResult();
            await transport.ReadyReplyReceived.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await transport.CutoverSendStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_cancelled_target_drain_rejects_new_prepare_and_preserves_active_slot()
    {
        var trace = new RelocationBehaviorTrace();
        var transport = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: true);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-drain-race-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-drain-race-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);
        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await transport.PrepareCallStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            transport.ReleasePrepareCall.TrySetResult();
            await transport.TargetPreparedBeforeReadySend.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            transport.ReleaseTargetReadySend.TrySetResult();
            await transport.ReadyReplyReceived.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await transport.CutoverSendStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            var before = GetTargetAttemptState(target.Runtime);
            Assert.Equal(1, before.Count);
            Assert.Equal(1, before.Staged);
            var rejectedDistinctAttempt = await Assert.ThrowsAsync<InvalidOperationException>(
                async () => await transport.PrepareNextTargetAttemptAsync(
                    CancellationToken.None));
            Assert.Contains(
                "already has an active local instance",
                rejectedDistinctAttempt.Message,
                StringComparison.Ordinal);
            Assert.Equal(before.Count, GetTargetAttemptState(target.Runtime).Count);
            transport.ReleaseCutoverSend.TrySetResult();
            await trace.WaitAsync("targetLifecycleStarted");

            using var drainCancellation = new CancellationTokenSource();
            var drain = InvokeActorGenerationResetAsync(
                target.Runtime,
                drainCancellation.Token);
            Assert.True(SpinWait.SpinUntil(
                () => GetTargetAttemptState(target.Runtime).Closing == 1,
                TimeSpan.FromSeconds(3)));

            var newAttemptFailure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await transport.PrepareNextTargetAttemptAsync(
                        CancellationToken.None)
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(3)));
            Assert.Equal(
                ZLinkFrameworkErrorKind.Unavailable,
                newAttemptFailure.Kind);
            Assert.Equal(before.Count, GetTargetAttemptState(target.Runtime).Count);

            drainCancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await drain);
            var after = GetTargetAttemptState(target.Runtime);
            Assert.Equal(before.Count, after.Count);
            Assert.Equal(before.Staged, after.Staged);
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_uses_ordered_one_way_cutover_before_target_CAS()
    {
        var trace = new RelocationBehaviorTrace();
        var cutover = new CanonicalRelocationTransportProbe(trace);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-cutover-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-cutover-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: cutover);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: cutover);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        var join = client.RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        var saved = client.RequestToActor(actorId, new BehaviorWork("saved"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await cutover.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.DoesNotContain("targetLifecycleStarted", trace.Events);

            var prefix = client.RequestToActor(actorId, new BehaviorWork("prefix"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await Task.Delay(100);
            cutover.ReleasePrepareCall.TrySetResult();

            await cutover.ReadyReplyReceived.Task.WaitAsync(TimeSpan.FromSeconds(3));
            await cutover.CutoverSendStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.True(cutover.DataSendCount > 0);
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.DoesNotContain("targetLifecycleStarted", trace.Events);

            cutover.ReleaseCutoverSend.TrySetResult();
            await cutover.CutoverSendSubmitted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            await trace.WaitAsync("targetLifecycleStarted");
            var temporary = client.RequestToActor(
                    actorId,
                    new BehaviorWork("temporary"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await Task.Delay(100);
            Assert.DoesNotContain("publicJoinCompleted", trace.Events);
            Assert.Empty(trace.DeliveredMarkers);

            trace.ReleaseTargetLifecycle.TrySetResult();
            await trace.WaitAsync("targetLifecycleCompleted");
            await cutover.SourceLeaveSubmitted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await trace.WaitAsync("publicJoinCompleted");
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);
            await trace.WaitAsync("sourceMembershipLeaveStarted");
            trace.ReleaseSourceLeave.TrySetResult();
            _ = await saved.WaitAsync(TimeSpan.FromSeconds(10));
            _ = await prefix.WaitAsync(TimeSpan.FromSeconds(10));
            _ = await temporary.WaitAsync(TimeSpan.FromSeconds(10));
            await target.Runtime.WaitForAcceptedActorHandoffsAsync(
                    CancellationToken.None)
                .WaitAsync(TimeSpan.FromSeconds(2));

            Assert.Equal(
                new[] { "saved", "prefix", "temporary" },
                trace.DeliveredMarkers);
            Assert.Equal("target", trace.PublicJoinCompletionNode);
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
            var orderedEvents = trace.Events;
            Assert.True(
                Array.IndexOf(orderedEvents.ToArray(), "targetLifecycleCompleted")
                < Array.IndexOf(orderedEvents.ToArray(), "sourceLeaveOneWaySubmitted"));
            Assert.True(
                Array.IndexOf(orderedEvents.ToArray(), "sourceLeaveOneWaySubmitted")
                < Array.IndexOf(orderedEvents.ToArray(), "publicJoinCompleted"));
            Assert.True(
                Array.IndexOf(orderedEvents.ToArray(), "publicJoinCompleted")
                < Array.IndexOf(orderedEvents.ToArray(), "savedWorkAdmitted"));
            Assert.Equal(1, trace.Events.Count(
                static value => value == "publicJoinCompleted"));
            var authorityMutations = trace.TargetAuthorityMutationCount;
            await cutover.ReplayCutoverAsync(CancellationToken.None);
            await Task.Delay(100);
            Assert.Equal(authorityMutations, trace.TargetAuthorityMutationCount);
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
            Assert.Equal(1, trace.Events.Count(
                static value => value == "publicJoinCompleted"));
        }
        finally
        {
            cutover.ReleasePrepareCall.TrySetResult();
            cutover.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_target_fallback_commits_after_one_second_and_late_cutover_is_inert()
    {
        var trace = new RelocationBehaviorTrace();
        var cutover = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: true);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-fallback-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-fallback-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: cutover);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: cutover);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        var join = client.RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        var saved = client.RequestToActor(actorId, new BehaviorWork("saved"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await cutover.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            cutover.ReleasePrepareCall.TrySetResult();
            await cutover.TargetPreparedBeforeReadySend.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await Task.Delay(TimeSpan.FromMilliseconds(1_100));
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.DoesNotContain("targetLifecycleStarted", trace.Events);
            cutover.ReleaseTargetReadySend.TrySetResult();
            await cutover.ReadyReplyReceived.Task.WaitAsync(TimeSpan.FromSeconds(3));
            await cutover.CutoverSendStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.False(cutover.CutoverSendSubmitted.Task.IsCompleted);
            Assert.False(trace.HasTargetAuthorityMutation);

            cutover.FailNextTargetReadySend();
            var duplicate = cutover.RetryPrepareAsync(
                    CancellationToken.None,
                    TimeSpan.FromMilliseconds(500))
                .AsTask();
            await cutover.TargetReadyFailureInjected.Task.WaitAsync(
                TimeSpan.FromSeconds(3));

            var beforeFallback = TimeSpan.FromMilliseconds(900)
                                 - Stopwatch.GetElapsedTime(
                                     cutover.ReadyReplyTimestamp);
            if (beforeFallback > TimeSpan.Zero)
                await Task.Delay(beforeFallback);
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.DoesNotContain("targetLifecycleStarted", trace.Events);

            var duplicateFailure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await duplicate);
            Assert.Equal(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                duplicateFailure.Kind);

            await trace.WaitForTargetAuthorityMutationAsync();
            await trace.WaitAsync("targetLifecycleStarted");
            Assert.False(cutover.CutoverSendSubmitted.Task.IsCompleted);
            var fallbackDelay = Stopwatch.GetElapsedTime(
                cutover.ReadyReplyTimestamp,
                trace.TargetAuthorityMutationTimestamp);
            Assert.InRange(
                fallbackDelay,
                TimeSpan.FromMilliseconds(900),
                TimeSpan.FromSeconds(3));

            trace.ReleaseTargetLifecycle.TrySetResult();
            await cutover.SourceLeaveSubmitted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await trace.WaitAsync("publicJoinCompleted");
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);
            await trace.WaitAsync("sourceMembershipLeaveStarted");

            var authorityMutations = trace.TargetAuthorityMutationCount;
            var lifecycleAttempts = trace.TargetLifecycleAttemptCount;
            var publicCompletions = trace.Events.Count(
                static value => value == "publicJoinCompleted");

            cutover.ReleaseCutoverSend.TrySetResult();
            await cutover.CutoverSendSubmitted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            await Task.Delay(100);
            Assert.Equal(authorityMutations, trace.TargetAuthorityMutationCount);
            Assert.Equal(lifecycleAttempts, trace.TargetLifecycleAttemptCount);
            Assert.Equal(
                publicCompletions,
                trace.Events.Count(static value => value == "publicJoinCompleted"));

            await cutover.ReplayCutoverAsync(CancellationToken.None);
            await Task.Delay(100);
            Assert.Equal(authorityMutations, trace.TargetAuthorityMutationCount);
            Assert.Equal(lifecycleAttempts, trace.TargetLifecycleAttemptCount);
            Assert.Equal(
                publicCompletions,
                trace.Events.Count(static value => value == "publicJoinCompleted"));

            trace.ReleaseSourceLeave.TrySetResult();
            _ = await saved.WaitAsync(TimeSpan.FromSeconds(10));
        }
        finally
        {
            cutover.ReleasePrepareCall.TrySetResult();
            cutover.ReleaseTargetReadySend.TrySetResult();
            cutover.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_target_ready_submit_failure_reuses_staging_on_exact_prepare_retry()
    {
        var trace = new RelocationBehaviorTrace();
        var transport = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: true,
            failTargetReadyOnce: true);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-ready-failure-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-ready-failure-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await transport.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
            transport.ReleasePrepareCall.TrySetResult();
            await transport.TargetReadyFailureInjected.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            Assert.Equal(0, transport.TargetAbortCallCount);
            Assert.False(trace.HasTargetAuthorityMutation);
            var retry = transport.RetryPrepareAsync(
                    CancellationToken.None,
                    TimeSpan.FromSeconds(3))
                .AsTask();
            await transport.TargetPreparedBeforeReadySend.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            transport.ReleaseTargetReadySend.TrySetResult();
            _ = await retry.WaitAsync(TimeSpan.FromSeconds(3));
            await transport.ReadyReplyReceived.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.True(target.Runtime.TryGetCreatedActorState(
                actorId,
                RelocationBehaviorHost.ActorType,
                out _));
            await trace.WaitAsync("targetLifecycleStarted");
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetAbortRegistration.TrySetResult();
            transport.ReleaseTargetRollbackDestroy.TrySetResult();
            transport.ReleaseRetriedTargetRollbackDestroy.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.FailHeldTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_force_stop_cancels_held_target_rollback_without_losing_terminal_owner()
    {
        var trace = new RelocationBehaviorTrace();
        var transport = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: true,
            failTargetReadyOnce: true,
            holdRetriedTargetRollbackDestroy: true);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-force-abort-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-force-abort-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);
        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        trace.TargetNodeRid = target.LocalNodeRid;
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetAdmissionReplied");

        try
        {
            await transport.PrepareCallStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            transport.ReleasePrepareCall.TrySetResult();
            await transport.TargetReadyFailureInjected.Task.WaitAsync(
                TimeSpan.FromSeconds(3));
            //  Spec 15 §4.2 / spec 52 §3.2: a failed READY submission is
            //  retryable submission state, not a relocation failure — the
            //  target keeps the exact prepared stage (removed only by an
            //  explicit abort or preparation-validity expiry), so no rollback
            //  destroy may start here.
            Assert.False(transport.TargetRollbackDestroyStarted.Task.IsCompleted);
            transport.ReleaseTargetReadySend.TrySetResult();
            using var retryDeadline = new CancellationTokenSource(
                TimeSpan.FromSeconds(3));
            _ = await RetryTargetPrepareAfterCleanupAsync(
                transport,
                retryDeadline.Token);
            //  Explicit abort of the retained stage drives the target
            //  rollback destroy, which the probe holds (this constructor
            //  keeps ReleaseTargetRollbackDestroy unset) so force stop finds
            //  it in flight.
            await transport.AbortRetriedTargetPrepareAsync();
            await transport.TargetRollbackDestroyStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(3));

            var forceStop = target.Services
                .GetRequiredService<IZLinkFrameworkRuntime>()
                .ShutdownAsync(TimeSpan.FromMilliseconds(100))
                .AsTask();
            var result = await forceStop.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(
                ZLinkFrameworkTerminationOutcome.ForceStopped,
                result.Outcome);
            Assert.False(target.Runtime.IsStarted);
            Assert.False(
                transport.TargetRollbackDestroyCompleted.Task.IsCompleted);
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetAbortRegistration.TrySetResult();
            transport.ReleaseTargetRollbackDestroy.TrySetResult();
            transport.ReleaseRetriedTargetRollbackDestroy.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.FailHeldTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_target_callback_push_converges_after_completed_without_waiting_for_discovery_poll()
    {
        var trace = new RelocationBehaviorTrace
        {
            SendTargetLifecyclePush = true
        };
        var transport = new CanonicalRelocationTransportProbe(trace);
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-push-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-push-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false);
        var sourceFailure = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        source.Runtime.ErrorSink.UnhandledCallbackException +=
            exception => sourceFailure.TrySetResult(exception);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        var stream = new RelocationBehaviorStream(
            RoutingId.From($"behavior-session-{Guid.NewGuid():N}"));
        var session = new ZLinkSessionContext(
            source.Runtime,
            stream,
            new RelocationBehaviorSessionHandlers(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        _ = await session.Actors.BindAsync(created.Actor);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            pollingInterval: TimeSpan.FromSeconds(3),
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        //  Regression pin (6fa7d6aab8): the direct remote-join cutover once
        //  activated the canonical-maintenance replay against the remote-join
        //  import, threw, and died as a detached task whose only surface was
        //  a target-side ProtocolError — the session route was never
        //  committed and the cross-node push (actor node != session node)
        //  vanished without any error. The target monitor must therefore end
        //  this join with no new protocol errors.
        using var targetMonitor = target.Runtime
            .GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
            .Node.OpenMeshMonitor();
        var targetProtocolErrorsBeforeJoin =
            targetMonitor.Status().ProtocolErrors;

        var join = source.Services.GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));
        await trace.WaitAsync("targetLifecycleStarted");
        trace.ReleaseTargetLifecycle.TrySetResult();
        await trace.WaitAsync("publicJoinCompleted");
        Assert.Equal(
            1,
            trace.Events.Count(static value => value == "publicJoinCompleted"));
        await target.Runtime.WaitForAcceptedActorHandoffsAsync(
                CancellationToken.None)
            .WaitAsync(TimeSpan.FromSeconds(1));

        try
        {
            await transport.RemoteSessionPushSubmitted.Task.WaitAsync(
                TimeSpan.FromSeconds(1));
            await transport.SessionRouteStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(1));
            await transport.SessionRouteCompleted.Task.WaitAsync(
                TimeSpan.FromSeconds(1));
            var delivery = await Task.WhenAny(
                    stream.FirstWrite.Task,
                    sourceFailure.Task)
                .WaitAsync(TimeSpan.FromSeconds(1));
            if (ReferenceEquals(delivery, sourceFailure.Task))
                throw await sourceFailure.Task;
            Assert.Equal(1, stream.WriteCount);
            Assert.True(source.Runtime.TryGetSessionActorBinding(
                actorId,
                out var routedBinding));
            Assert.NotNull(routedBinding.AppliedCanonicalRelocationRoute);
            Assert.Equal(
                targetProtocolErrorsBeforeJoin,
                targetMonitor.Status().ProtocolErrors);
        }
        finally
        {
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    [Fact]
    public async Task ActorJoin_runtime_preserves_observable_order_and_exact_callback_counts()
    {
        using var fixture = LoadBehaviorFixture();
        Assert.Equal(
            "zlink.framework.relocation-behavior",
            fixture.RootElement.GetProperty("fixture").GetString());

        var trace = new RelocationBehaviorTrace();
        //  A pass-through probe: every gate is released up front, so the
        //  runtime is unmodified — it only records the one-way OnLeaveActor
        //  submission the fixture's "sourceMembershipLeaveSubmitted" event
        //  stands for.
        var transport = new CanonicalRelocationTransportProbe(trace);
        transport.ReleasePrepareCall.TrySetResult();
        transport.ReleaseCutoverSend.TrySetResult();
        var locationStore = new RecordingLocationStore(
            new ZLinkInMemoryProviderLocationStore(),
            trace);
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"behavior-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"behavior-spot-{Guid.NewGuid():N}";
        trace.ActorId = actorId;

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: false,
            canonicalTransportProbe: transport);
        var actorManager = source.Services.GetRequiredService<IZLinkActorManager>();
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await actorManager.GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async());
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        trace.SourceNodeRid = created.Actor.NodeRid;
        await locationStore.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            locationStore,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport);
        await WaitUntilAsync(
            () => source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1
                  && target.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
                      .Node.Status().ActivePeerCount == 1);

        var spot = await source.Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);
        trace.TargetNodeRid = target.LocalNodeRid;

        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        var join = client.RequestToActor(
                actorId,
                new BeginBehaviorJoin(targetSpotId))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        await trace.WaitAsync("relocationRequested");

        var saved = client.RequestToActor(actorId, new BehaviorWork("saved"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        Assert.False(saved.IsCompleted);
        trace.ReleaseJoinHandler.TrySetResult();
        _ = await join.WaitAsync(TimeSpan.FromSeconds(15));

        await trace.WaitAsync("targetLifecycleStarted");
        var temporary = client.RequestToActor(
                actorId,
                new BehaviorWork("temporary"))
            .Timeout(TimeSpan.FromSeconds(15))
            .Async<BehaviorAck>()
            .AsTask();
        try
        {
        await Task.Delay(50);
        Assert.DoesNotContain(
            trace.Events,
            value => value is "sourceMembershipLeaveSubmitted"
                or "publicJoinCompleted"
                or "savedWorkAdmitted"
                or "temporaryWorkAdmitted");

        trace.ReleaseTargetLifecycle.TrySetResult();
        await trace.WaitAsync("sourceMembershipLeaveSubmitted");
        var completionLeftCanonicalAuthority = false;
        try
        {
            await trace.WaitAsync("publicJoinCompleted");
            var completedAuthority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(locationStore)
                    .ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)));
            completionLeftCanonicalAuthority =
                ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    completedAuthority.Snapshot.Payload.Span,
                    out _);
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);
            _ = await saved.WaitAsync(TimeSpan.FromSeconds(15));
            _ = await temporary.WaitAsync(TimeSpan.FromSeconds(15));
            await trace.WaitForTargetAuthorityAsync();
            Assert.Equal(1, trace.TargetLifecycleAttemptCount);
            Assert.False(trace.ReleaseSourceLeave.Task.IsCompleted);

            var direct = await client.RequestToActor(
                    actorId,
                    new BehaviorWork("direct"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>();
            Assert.Equal("direct", direct.Marker);
        }
        finally
        {
            trace.ReleaseSourceLeave.TrySetResult();
        }

        await WaitUntilAsync(async () =>
        {
            var current = await actorManager.FindAsync(actorId);
            return current is { } actor && actor.NodeRid == target.LocalNodeRid;
        });

        Assert.Equal(
            ["saved", "temporary", "direct"],
            trace.DeliveredMarkers);
        Assert.Equal(trace.SourceObjectGeneration, trace.TargetObjectGeneration);
        Assert.True(trace.TargetOwnerGeneration > trace.SourceOwnerGeneration);
        Assert.False(
            completionLeftCanonicalAuthority,
            "An unbound ActorJoin must normalize the completed relocation "
            + "before the public join completion can outlive its target process.");
        AssertObservedBehavior(fixture.RootElement, trace.Events);
        }
        finally
        {
            trace.ReleaseTargetLifecycle.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
        }
    }

    private static void AssertObservedBehavior(
        JsonElement fixture,
        IReadOnlyList<string> events)
    {
        string[] observedRequired =
        [
            "relocationRequested",
            "sourceStateCaptured",
            "targetStateRestored",
            "targetLifecycleCompleted",
            "sourceMembershipLeaveSubmitted",
            "publicJoinCompleted",
            "savedWorkAdmitted",
            "temporaryWorkAdmitted"
        ];
        var actorJoin = fixture.GetProperty("profiles")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == "actorJoin");
        var required = fixture.GetProperty("commonLifecycle")
            .GetProperty("requiredEvents")
            .EnumerateArray()
            .Select(static item => item.GetString()!)
            .Concat(actorJoin.GetProperty("requiredEvents")
                .EnumerateArray()
                .Select(static item => item.GetString()!))
            .ToArray();
        Assert.All(observedRequired, name => Assert.Contains(name, required));
        Assert.All(
            observedRequired,
            name => Assert.Equal(1, events.Count(value => value == name)));

        var order = fixture.GetProperty("commonLifecycle")
            .GetProperty("requiredOrder")
            .EnumerateArray()
            .Concat(actorJoin.GetProperty("additionalOrder").EnumerateArray())
            .Select(edge => edge.EnumerateArray()
                .Select(static item => item.GetString()!)
                .ToArray())
            .ToArray();
        foreach (var before in observedRequired)
        {
            foreach (var after in observedRequired)
            {
                if (!IsRequiredBefore(before, after, order)) continue;
                Assert.True(
                    events.IndexOf(before) < events.IndexOf(after),
                    $"Expected {before} before {after}: {string.Join(',', events)}");
            }
        }

        var counts = actorJoin.GetProperty("callbackCounts");
        Assert.Equal(
            counts.GetProperty("targetMembershipJoin").GetInt32(),
            events.Count(value => value == "targetMembershipJoinCallback"));
        Assert.Equal(
            counts.GetProperty("sourceMembershipLeave").GetInt32(),
            events.Count(value => value == "sourceMembershipLeaveSubmitted"));
        var terminal = actorJoin.GetProperty("terminalEvent").GetString()!;
        Assert.Equal(1, events.Count(value => value == terminal));
    }

    private static bool IsRequiredBefore(
        string before,
        string after,
        IReadOnlyList<string[]> order)
    {
        if (string.Equals(before, after, StringComparison.Ordinal)) return false;
        var pending = new Queue<string>();
        var visited = new HashSet<string>(StringComparer.Ordinal) { before };
        pending.Enqueue(before);
        while (pending.TryDequeue(out var current))
        {
            foreach (var edge in order.Where(edge => edge[0] == current))
            {
                if (edge[1] == after) return true;
                if (visited.Add(edge[1])) pending.Enqueue(edge[1]);
            }
        }
        return false;
    }

    private static JsonDocument LoadBehaviorFixture()
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "relocation-behavior-v1.json");
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private static async Task WaitUntilAsync(Func<bool> predicate) =>
        await WaitUntilAsync(() => ValueTask.FromResult(predicate()));

    private static Task InvokeActorGenerationResetAsync(
        ZLinkFrameworkRuntime runtime,
        CancellationToken cancellationToken)
    {
        var reset = typeof(ZLinkFrameworkRuntime).GetMethod(
            "ResetActorRuntimeGenerationAsync",
            BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException(
                "Actor generation reset owner was not found.");
        var operation = Assert.IsType<ValueTask>(reset.Invoke(
            runtime,
            [cancellationToken, null]));
        return operation.AsTask();
    }

    private static (int Count, int Staged, int Closing) GetTargetAttemptState(
        ZLinkFrameworkRuntime runtime)
    {
        var owner = runtime.StandaloneActorRelocationRuntime;
        var attemptsField = owner.GetType().GetField(
            "_targetAttempts",
            BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException(
                "Target attempt owner was not found.");
        var attempts = attemptsField.GetValue(owner)
                       ?? throw new InvalidOperationException(
                           "Target attempt owner is unavailable.");
        var type = attempts.GetType();
        var count = Assert.IsType<int>(type.GetProperty("Count")!.GetValue(attempts));
        var values = Assert.IsAssignableFrom<System.Collections.IEnumerable>(
            type.GetProperty("Values")!.GetValue(attempts));
        var staged = 0;
        var closing = 0;
        foreach (var slot in values)
        {
            var slotType = slot!.GetType();
            if (slotType.GetProperty(
                    "Stage",
                    BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(slot) is not null)
                staged++;
            var leases = slotType.GetField(
                    "_leases",
                    BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(slot)!;
            if (Assert.IsType<bool>(leases.GetType().GetField(
                        "_closing",
                        BindingFlags.Instance | BindingFlags.NonPublic)!
                    .GetValue(leases)))
                closing++;
        }
        return (count, staged, closing);
    }

    private static async Task<ZLinkServiceWireCodec.RelocationReadyRecord>
        RetryTargetPrepareAfterCleanupAsync(
            CanonicalRelocationTransportProbe transport,
            CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                return await transport.RetryTargetPrepareAsync(
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
            {
                await Task.Yield();
            }
        }
    }

    private static async Task WaitUntilAsync(Func<ValueTask<bool>> predicate)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            if (await predicate()) return;
            await Task.Delay(10);
        }
        Assert.True(await predicate(), "The runtime did not reach the expected behavior state.");
    }
}

internal sealed class RelocationBehaviorTrace
{
    private readonly object _gate = new();
    private readonly List<string> _events = [];
    private readonly List<string> _deliveredMarkers = [];
    private readonly ConcurrentDictionary<string, TaskCompletionSource> _signals =
        new(StringComparer.Ordinal);

    internal string? ActorId { get; set; }
    internal RoutingId SourceNodeRid { get; set; }
    internal RoutingId TargetNodeRid { get; set; }
    internal ulong SourceObjectGeneration { get; set; }
    internal ulong TargetObjectGeneration { get; private set; }
    internal ulong SourceOwnerGeneration { get; private set; }
    internal ulong TargetOwnerGeneration { get; private set; }
    internal int TargetLifecycleAttemptCount;
    internal int TargetAuthorityMutationCount;
    internal long TargetAuthorityMutationTimestamp;
    internal bool SendTargetLifecyclePush { get; init; }
    internal string? PublicJoinCompletionNode { get; private set; }
    internal bool HasTargetAuthorityMutation =>
        Volatile.Read(ref TargetAuthorityMutationCount) != 0;
    internal TaskCompletionSource ReleaseJoinHandler { get; } = Signal();
    internal TaskCompletionSource ReleaseTargetLifecycle { get; } = Signal();
    internal TaskCompletionSource ReleaseSourceLeave { get; } = Signal();
    private TaskCompletionSource TargetAuthorityObserved { get; } = Signal();
    private TaskCompletionSource TargetAuthorityMutationObserved { get; } = Signal();

    internal IReadOnlyList<string> Events
    {
        get { lock (_gate) return _events.ToArray(); }
    }

    internal IReadOnlyList<string> DeliveredMarkers
    {
        get { lock (_gate) return _deliveredMarkers.ToArray(); }
    }

    internal void Record(string name)
    {
        lock (_gate) _events.Add(name);
        _signals.GetOrAdd(name, static _ => Signal()).TrySetResult();
    }

    internal void RecordDelivery(string marker, string node)
    {
        Assert.Equal("target", node);
        lock (_gate) _deliveredMarkers.Add(marker);
        Record(marker switch
        {
            "saved" => "savedWorkAdmitted",
            "prefix" => "prefixWorkAdmitted",
            "temporary" => "temporaryWorkAdmitted",
            "direct" => "directWorkAdmitted",
            _ => throw new InvalidOperationException($"Unknown marker '{marker}'.")
        });
    }

    internal async Task WaitAsync(string name) =>
        await _signals.GetOrAdd(name, static _ => Signal()).Task
            .WaitAsync(TimeSpan.FromSeconds(15));

    internal async Task WaitForTargetAuthorityAsync() =>
        await TargetAuthorityObserved.Task.WaitAsync(TimeSpan.FromSeconds(15));

    internal async Task WaitForTargetAuthorityMutationAsync() =>
        await TargetAuthorityMutationObserved.Task.WaitAsync(TimeSpan.FromSeconds(15));

    internal void RecordPublicJoinCompletion(string node)
    {
        lock (_gate) PublicJoinCompletionNode = node;
        Record("publicJoinCompleted");
    }

    internal void ObserveActorAuthority(ZLinkAuthoritySnapshot snapshot)
    {
        ObserveReadyActorAuthority(snapshot);
        if (SourceOwnerGeneration == 0
            || snapshot.AuthorityOwnerGeneration <= SourceOwnerGeneration)
            return;
        if ((!ZLinkActorAuthorityPayloadCodec.TryDecode(
                    snapshot.Payload.Span,
                    out var authority)
             && !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    snapshot.Payload.Span,
                    out authority))
            || authority.ActorId != ActorId
            || authority.NodeRid != TargetNodeRid)
            return;
        Interlocked.Increment(ref TargetAuthorityMutationCount);
        Interlocked.CompareExchange(
            ref TargetAuthorityMutationTimestamp,
            Stopwatch.GetTimestamp(),
            0);
        TargetAuthorityMutationObserved.TrySetResult();
    }

    internal void ObserveReadyActorAuthority(ZLinkAuthoritySnapshot snapshot)
    {
        if (!ZLinkActorAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.State != ZLinkActorAuthorityState.Ready
            || authority.ActorId != ActorId)
            return;
        if (authority.NodeRid == SourceNodeRid)
        {
            SourceOwnerGeneration = snapshot.AuthorityOwnerGeneration;
            return;
        }
        if (authority.NodeRid != TargetNodeRid) return;
        TargetObjectGeneration = snapshot.ObjectGeneration;
        TargetOwnerGeneration = snapshot.AuthorityOwnerGeneration;
        TargetAuthorityObserved.TrySetResult();
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class RecordingLocationStore(
    IZLinkLocationStore inner,
    RelocationBehaviorTrace trace) : IZLinkLocationStore
{
    private readonly ZLinkProviderLocationRepository _reader = new(inner);

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default) =>
        inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        var result = await inner.WriteAsync(request, cancellationToken);
        if (result is ZLinkStoreWriteResult.Applied && trace.ActorId is { } actorId)
        {
            var read = await _reader.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                cancellationToken);
            if (read is ZLinkAuthorityReadResult.Found found)
                trace.ObserveActorAuthority(found.Snapshot);
        }
        return result;
    }

    internal async ValueTask ObserveActorAuthorityAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        var read = await _reader.ReadAuthorityAsync(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
            cancellationToken);
        if (read is ZLinkAuthorityReadResult.Found found)
            trace.ObserveActorAuthority(found.Snapshot);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);
}

internal sealed class SynchronizedRelocationStore :
    IZLinkRelocationRepository,
    IZLinkRelocationStore
{
    private readonly object _gate = new();
    private readonly InMemoryRelocationStore _inner = new();

    public ValueTask<ZLinkBlobPutResult> PutAsync(ZLinkBlobReference reference, ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutAsync(reference, payload, retention, cancellationToken); }
    public ValueTask<ZLinkBlobReadResult> ReadAsync(ZLinkBlobReference reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.ReadAsync(reference, cancellationToken); }
    public ValueTask<ZLinkBlobRenewResult> RenewAsync(ZLinkBlobReference reference, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.RenewAsync(reference, retention, cancellationToken); }
    public ValueTask DeleteAsync(ZLinkBlobReference reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.DeleteAsync(reference, cancellationToken); }
    public ValueTask<ZLinkRelocationStored> PutRelocationAsync(ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutRelocationAsync(payload, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(string reference, ReadOnlyMemory<byte> payload, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.PutRelocationAtAsync(reference, payload, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(string reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.GetRelocationAsync(reference, cancellationToken); }
    public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(string reference, TimeSpan retention, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.RenewRelocationAsync(reference, retention, cancellationToken); }
    public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(string reference, CancellationToken cancellationToken = default)
    { lock (_gate) return _inner.DeleteRelocationAsync(reference, cancellationToken); }
}

internal sealed class RelocationBehaviorHost : IAsyncDisposable
{
    internal const string MeshName = "relocation-behavior";
    internal const string ActorType = "relocation-behavior-actor";
    internal const string SpotType = "relocation-behavior-spot";

    private readonly ServiceProvider _provider;
    private readonly IHostedService _hosted;

    private RelocationBehaviorHost(ServiceProvider provider, IHostedService hosted)
    {
        _provider = provider;
        _hosted = hosted;
        Runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
    }

    internal IServiceProvider Services => _provider;
    internal ZLinkFrameworkRuntime Runtime { get; }
    internal RoutingId LocalNodeRid => Runtime.GetMeshNodeRuntime(MeshName).Node.RoutingId;

    internal static async Task<RelocationBehaviorHost> StartAsync(
        string node,
        RelocationBehaviorTrace trace,
        IZLinkLocationStore locationStore,
        IZLinkRelocationStore relocationStore,
        bool registerTargetSpot,
        TimeSpan? pollingInterval = null,
        CanonicalRelocationTransportProbe? canonicalTransportProbe = null)
    {
        var services = new ServiceCollection();
        if (canonicalTransportProbe is not null)
            services.AddSingleton<IZLinkBackendAdapterFactory>(
                new ProbedBackendAdapterFactory(
                    new ZLinkDotNetBackendAdapterFactory(),
                    canonicalTransportProbe));
        services.AddSingleton(trace);
        services.AddSingleton(new BehaviorNode(node));
        services.AddTransient<BeginBehaviorJoinHandler>();
        services.AddTransient<EntryBehaviorWorkHandler>();
        services.AddTransient<TargetBehaviorWorkHandler>();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(locationStore);
            options.AddRelocationStore(relocationStore);
            options.ConfigureLocations().PollingInterval =
                pollingInterval ?? TimeSpan.FromMilliseconds(10);
            var objects = options.AddRouteMesh(MeshName)
                .Listen(ReserveTcpEndpoint())
                .SetRoutingIdPrefix($"behavior-{node}")
                .SetActorLimit(100)
                .SetSpotLimit(100)
                .Objects()
                .Server()
                .AddEntrySpot<BehaviorEntrySpot>()
                .AddActorFactory<BehaviorActor, BehaviorActorFactory>(
                    ActorType,
                    factory => factory.PreserveStateWith<BehaviorActorRelocationAdapter>());
            if (registerTargetSpot)
                objects.AddSpotFactory<BehaviorTargetSpot>(
                    SpotType,
                    factory => factory.DisableRelocation());
        });
        var provider = services.BuildServiceProvider();
        var hosted = provider.GetServices<IHostedService>().Single(
            static service => service is ZLinkFrameworkHostedService);
        try
        {
            await hosted.StartAsync(CancellationToken.None);
            return new RelocationBehaviorHost(provider, hosted);
        }
        catch
        {
            await provider.DisposeAsync();
            throw;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Runtime.IsStarted)
            await _hosted.StopAsync(CancellationToken.None);
        await _provider.DisposeAsync();
    }

    internal Task StopAsync() =>
        _hosted.StopAsync(CancellationToken.None);

    private static string ReserveTcpEndpoint()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return $"tcp://127.0.0.1:{port}";
    }
}

internal sealed record BehaviorNode(string Name);
internal sealed record BehaviorCreate(int State);
internal sealed record BeginBehaviorJoin(string TargetSpotId);
internal sealed record BehaviorWork(string Marker);
internal sealed record BehaviorAck(string Marker);
internal sealed record BehaviorLifecyclePush(string Marker);

internal sealed class BehaviorActor(
    string actorId,
    IZLinkActorContext context,
    RelocationBehaviorTrace trace,
    BehaviorNode node) : IZLinkActor
{
    internal int State { get; set; }
    public string ActorId { get; } = actorId;
    public IZLinkActorContext Context { get; } = context;

    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion);
        trace.RecordPublicJoinCompletion(node.Name);
        return ValueTask.CompletedTask;
    }
}

internal sealed class BehaviorActorFactory(
    RelocationBehaviorTrace trace,
    BehaviorNode node)
    : IZLinkActorFactory<BehaviorActor>
{
    public ValueTask<BehaviorActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(new BehaviorActor(context.ActorId, context, trace, node));
}

internal sealed class BehaviorActorRelocationAdapter(
    RelocationBehaviorTrace trace,
    BehaviorNode node) : IZLinkActorRelocationAdapter<BehaviorActor>
{
    public ValueTask<byte[]> CaptureAsync(
        BehaviorActor actor,
        CancellationToken cancellationToken)
    {
        Assert.Equal("source", node.Name);
        trace.Record("sourceStateCaptured");
        return ValueTask.FromResult(BitConverter.GetBytes(actor.State));
    }

    public ValueTask RestoreAsync(
        BehaviorActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        Assert.Equal("target", node.Name);
        actor.State = BitConverter.ToInt32(payload.Span);
        trace.Record("targetStateRestored");
        return ValueTask.CompletedTask;
    }
}

internal sealed class BehaviorEntrySpot(
    IZLinkEntrySpotContext context,
    RelocationBehaviorTrace trace,
    BehaviorNode node) : IZLinkEntrySpot<BehaviorActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<BeginBehaviorJoinHandler, BehaviorActor>(
            nameof(BeginBehaviorJoin));
        Context.Handlers.AddActorPacket<EntryBehaviorWorkHandler, BehaviorActor>(
            nameof(BehaviorWork));
    }

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        BehaviorActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        actor.State = request.Decode<BehaviorCreate>().State;
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        trace.Record("targetAdmissionReplied");
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public ValueTask OnJoinedActorAsync(BehaviorActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnLeaveActorAsync(BehaviorActor actor, CancellationToken cancellationToken)
    {
        Assert.Equal("source", node.Name);
        trace.Record("sourceMembershipLeaveStarted");
        await trace.ReleaseSourceLeave.Task.WaitAsync(cancellationToken);
    }
}

internal sealed class BehaviorTargetSpot(
    IZLinkSpotContext context,
    RelocationBehaviorTrace trace) : IZLinkSpot<BehaviorActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<TargetBehaviorWorkHandler, BehaviorActor>(
            nameof(BehaviorWork));

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(ZLinkMessage request, CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request, CancellationToken cancellationToken)
    {
        trace.Record("targetAdmissionReplied");
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public async ValueTask OnJoinedActorAsync(BehaviorActor actor, CancellationToken cancellationToken)
    {
        _ = Interlocked.Increment(ref trace.TargetLifecycleAttemptCount);
        trace.Record("targetMembershipJoinCallback");
        trace.Record("targetLifecycleStarted");
        await trace.ReleaseTargetLifecycle.Task.WaitAsync(cancellationToken);
        trace.Record("targetLifecycleCompleted");
        if (trace.SendTargetLifecyclePush)
            await actor.Context.BoundSession
                .Send(new BehaviorLifecyclePush("target-ready"))
                .Async(cancellationToken);
    }

    public ValueTask OnLeaveActorAsync(BehaviorActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class BeginBehaviorJoinHandler(RelocationBehaviorTrace trace)
    : IZLinkEntrySpotActorRequestHandler<BehaviorEntrySpot, BehaviorActor, BeginBehaviorJoin, BehaviorAck>
{
    public async ValueTask<BehaviorAck> HandleAsync(
        BehaviorEntrySpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        BeginBehaviorJoin request,
        CancellationToken cancellationToken)
    {
        trace.Record("relocationRequested");
        actor.Context.JoinSpot(request.TargetSpotId, request)
            .Timeout(TimeSpan.FromSeconds(15))
            .Defer();
        await trace.ReleaseJoinHandler.Task.WaitAsync(cancellationToken);
        return new BehaviorAck("join");
    }
}

internal sealed class EntryBehaviorWorkHandler(RelocationBehaviorTrace trace, BehaviorNode node)
    : IZLinkEntrySpotActorRequestHandler<BehaviorEntrySpot, BehaviorActor, BehaviorWork, BehaviorAck>
{
    public ValueTask<BehaviorAck> HandleAsync(BehaviorEntrySpot spot, BehaviorActor actor, IZLinkMessageContext context, BehaviorWork request, CancellationToken cancellationToken)
    {
        trace.RecordDelivery(request.Marker, node.Name);
        return ValueTask.FromResult(new BehaviorAck(request.Marker));
    }
}

internal sealed class TargetBehaviorWorkHandler(RelocationBehaviorTrace trace, BehaviorNode node)
    : IZLinkSpotActorRequestHandler<BehaviorTargetSpot, BehaviorActor, BehaviorWork, BehaviorAck>
{
    public ValueTask<BehaviorAck> HandleAsync(BehaviorTargetSpot spot, BehaviorActor actor, IZLinkMessageContext context, BehaviorWork request, CancellationToken cancellationToken)
    {
        trace.RecordDelivery(request.Marker, node.Name);
        return ValueTask.FromResult(new BehaviorAck(request.Marker));
    }
}

internal sealed class CanonicalRelocationTransportProbe
{
    private readonly RelocationBehaviorTrace _trace;
    private readonly bool _holdTargetReady;
    private readonly bool _holdTargetAbortRegistration;
    private readonly bool _holdRetriedTargetRollbackDestroy;
    private int _failTargetReadyOnce;
    private IZLinkBackendCanonicalRelocation? _prepareTransport;
    private ICanonicalRelocationTarget? _target;
    private RoutingId _prepareTargetNodeRid;
    private RoutingId _prepareSourceNodeRid;
    private ZLinkServiceWireCodec.RelocationPrepareRecord? _prepare;
    private ZLinkRelocationTransferPayload? _preparePayload;
    private TimeSpan _prepareTimeout;
    private IZLinkBackendCanonicalRelocation? _transport;
    private RoutingId _targetNodeRid;
    private ZLinkServiceWireCodec.RelocationCutoverRecord? _cutover;

    internal TaskCompletionSource PrepareCallStarted { get; } = Signal();
    internal TaskCompletionSource ReleasePrepareCall { get; } = Signal();
    internal TaskCompletionSource ReadyReplyReceived { get; } = Signal();
    internal TaskCompletionSource CutoverSendStarted { get; } = Signal();
    internal TaskCompletionSource ReleaseCutoverSend { get; } = Signal();
    internal TaskCompletionSource CutoverSendSubmitted { get; } = Signal();
    internal TaskCompletionSource SourceLeaveSubmitted { get; } = Signal();
    internal TaskCompletionSource RemoteSessionPushSubmitted { get; } = Signal();
    internal TaskCompletionSource TargetPreparedBeforeReadySend { get; } = Signal();
    internal TaskCompletionSource ReleaseTargetReadySend { get; } = Signal();
    internal TaskCompletionSource FailHeldTargetReadySend { get; } = Signal();
    internal TaskCompletionSource TargetReadyFailureInjected { get; } = Signal();
    internal TaskCompletionSource TargetAbortCallStarted { get; } = Signal();
    internal TaskCompletionSource DuplicateTargetPrepareRejected { get; } = Signal();
    internal TaskCompletionSource ReleaseTargetAbortRegistration { get; } = Signal();
    internal TaskCompletionSource TargetRollbackDestroyStarted { get; } = Signal();
    internal TaskCompletionSource ReleaseTargetRollbackDestroy { get; } = Signal();
    internal TaskCompletionSource TargetRollbackDestroyCompleted { get; } = Signal();
    internal TaskCompletionSource TargetPrepareAborted { get; } = Signal();
    internal TaskCompletionSource RetriedTargetRollbackDestroyStarted { get; } = Signal();
    internal TaskCompletionSource ReleaseRetriedTargetRollbackDestroy { get; } = Signal();
    internal TaskCompletionSource RetriedTargetRollbackDestroyCompleted { get; } = Signal();
    internal TaskCompletionSource SessionRouteStarted { get; } = Signal();
    internal TaskCompletionSource SessionRouteCompleted { get; } = Signal();
    internal long ReadyReplyTimestamp;
    internal ZLinkRemoteSessionPushRelay? RemoteSessionPush;
    internal ZLinkServiceWireCodec.SessionRelocationRouteRecord? SessionRoute;
    internal int DataSendCount;
    internal int TargetAbortCallCount => Volatile.Read(ref _targetAbortCallCount);
    private int _targetRollbackDestroyCount;
    private int _targetPrepareCallCount;
    private int _targetAbortCallCount;

    internal CanonicalRelocationTransportProbe(
        RelocationBehaviorTrace trace,
        bool holdTargetReady = false,
        bool failTargetReadyOnce = false,
        bool holdTargetAbortRegistration = false,
        bool holdRetriedTargetRollbackDestroy = false)
    {
        _trace = trace;
        _holdTargetReady = holdTargetReady;
        _holdTargetAbortRegistration = holdTargetAbortRegistration;
        _holdRetriedTargetRollbackDestroy = holdRetriedTargetRollbackDestroy;
        _failTargetReadyOnce = failTargetReadyOnce ? 1 : 0;
        if (!holdTargetAbortRegistration)
            ReleaseTargetAbortRegistration.TrySetResult();
        if (!failTargetReadyOnce)
            ReleaseTargetRollbackDestroy.TrySetResult();
        if (!holdRetriedTargetRollbackDestroy)
            ReleaseRetriedTargetRollbackDestroy.TrySetResult();
        if (!holdTargetReady)
            ReleaseTargetReadySend.TrySetResult();
    }

    internal void FailNextTargetReadySend() =>
        Interlocked.Exchange(ref _failTargetReadyOnce, 1);

    internal ICanonicalRelocationTarget WrapTarget(
        ICanonicalRelocationTarget target)
    {
        _target = target;
        return _holdTargetReady
            ? new ReadyGatedCanonicalRelocationTarget(target, this)
            : target;
    }

    internal ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        RetryTargetPrepareAsync(CancellationToken cancellationToken) =>
        (_target ?? throw new InvalidOperationException(
            "No canonical relocation target was captured."))
        .PrepareAsync(
            _prepare ?? throw new InvalidOperationException(
                "No canonical prepare was captured."),
            //  Direct transfer carries the schema relocation-envelope-v1
            //  logical stream. Decode it through the transfer payload codec
            //  so the probe follows the production wire boundary.
            ZLinkRelocationTransferPayload.DecodeEnvelope(
                (_preparePayload ?? throw new InvalidOperationException(
                    "No canonical prepare payload was captured.")).Encoded),
            _prepareSourceNodeRid,
            new ZLinkCanonicalRelocationPreparationLease(),
            cancellationToken);

    internal ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        PrepareNextTargetAttemptAsync(CancellationToken cancellationToken) =>
        (_target ?? throw new InvalidOperationException(
            "No canonical relocation target was captured."))
        .PrepareAsync(
            (_prepare ?? throw new InvalidOperationException(
                "No canonical prepare was captured.")) with
            {
                TargetAttemptGeneration = checked(
                    _prepare.TargetAttemptGeneration + 1)
            },
            ZLinkRelocationTransferPayload.DecodeEnvelope(
                (_preparePayload ?? throw new InvalidOperationException(
                    "No canonical prepare payload was captured.")).Encoded),
            _prepareSourceNodeRid,
            new ZLinkCanonicalRelocationPreparationLease(),
            cancellationToken);

    internal ValueTask AbortRetriedTargetPrepareAsync() =>
        (_target ?? throw new InvalidOperationException(
            "No canonical relocation target was captured."))
        .AbortPreparedAsync(
            _prepare ?? throw new InvalidOperationException(
                "No canonical prepare was captured."),
            _prepareSourceNodeRid);

    internal void SubmitRetriedTargetReady() =>
        (_target ?? throw new InvalidOperationException(
            "No canonical relocation target was captured."))
        .ReadySubmitted(
            _prepare ?? throw new InvalidOperationException(
                "No canonical prepare was captured."),
            _prepareSourceNodeRid);

    internal async ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        PrepareAsync(
            IZLinkBackendCanonicalRelocation transport,
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            ZLinkRelocationTransferPayload payload,
            TimeSpan timeout,
            CancellationToken cancellationToken)
    {
        _prepareTransport = transport;
        _prepareTargetNodeRid = targetNodeRid;
        _prepare = prepare;
        _preparePayload = payload;
        _prepareTimeout = timeout;
        PrepareCallStarted.TrySetResult();
        await ReleasePrepareCall.Task.WaitAsync(cancellationToken);
        var ready = await transport.PrepareCanonicalRelocationAsync(
                targetNodeRid,
                prepare,
                payload,
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
        Volatile.Write(ref ReadyReplyTimestamp, Stopwatch.GetTimestamp());
        ReadyReplyReceived.TrySetResult();
        return ready;
    }

    internal ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
        RetryPrepareAsync(
            CancellationToken cancellationToken,
            TimeSpan? timeout = null) =>
        _prepareTransport?.PrepareCanonicalRelocationAsync(
            _prepareTargetNodeRid,
            _prepare ?? throw new InvalidOperationException(
                "No canonical prepare was captured."),
            _preparePayload ?? throw new InvalidOperationException(
                "No canonical prepare payload was captured."),
            timeout ?? _prepareTimeout,
            cancellationToken)
        ?? throw new InvalidOperationException(
            "No canonical prepare transport was captured.");

    internal async ValueTask SendDataAsync(
        IZLinkBackendCanonicalRelocation transport,
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationDataRecord data,
        CancellationToken cancellationToken)
    {
        await transport.SendCanonicalRelocationDataAsync(
                targetNodeRid,
                data,
                cancellationToken)
            .ConfigureAwait(false);
        Interlocked.Increment(ref DataSendCount);
    }

    internal async ValueTask SendCutoverAsync(
        IZLinkBackendCanonicalRelocation transport,
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
        CancellationToken cancellationToken)
    {
        _transport = transport;
        _targetNodeRid = targetNodeRid;
        _cutover = cutover;
        CutoverSendStarted.TrySetResult();
        await ReleaseCutoverSend.Task.WaitAsync(cancellationToken);
        await transport.SendCanonicalRelocationCutoverAsync(
                targetNodeRid,
                cutover,
                cancellationToken)
            .ConfigureAwait(false);
        CutoverSendSubmitted.TrySetResult();
    }

    internal ValueTask ReplayCutoverAsync(CancellationToken cancellationToken) =>
        _transport?.SendCanonicalRelocationCutoverAsync(
            _targetNodeRid,
            _cutover ?? throw new InvalidOperationException(
                "No canonical cutover was captured."),
            cancellationToken)
        ?? throw new InvalidOperationException(
            "No canonical cutover transport was captured.");

    internal void RecordSourceLeaveSubmission()
    {
        _trace.Record("sourceLeaveOneWaySubmitted");
        //  Fixture vocabulary (relocation-behavior-v1.json): the
        //  "sourceMembershipLeaveSubmitted" behavior event is the one-way
        //  OnLeaveActor SUBMISSION (spec 15 §4.2 step 7 orders the submission before
        //  the Join completion callback; the cross-node execution of the
        //  source callback is unordered against target-local completion).
        _trace.Record("sourceMembershipLeaveSubmitted");
        SourceLeaveSubmitted.TrySetResult();
    }

    internal void RecordRemoteSessionPushSubmission() =>
        RemoteSessionPushSubmitted.TrySetResult();

    internal async ValueTask SendNodeAsync(
        IZLinkBackendSpotNode transport,
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        CancellationToken cancellationToken,
        ReadOnlyMemory<byte> metadata)
    {
        await transport.SendToNodeAsync(
                targetNodeRid,
                parts,
                flags,
                cancellationToken,
                metadata)
            .ConfigureAwait(false);
        if (ZLinkEnvelopeCodec.DecodeHeader(parts).MessageName
            == ZLinkRemoteSessionPushProtocol.PacketName)
        {
            RemoteSessionPush = Assert.IsType<ZLinkRemoteSessionPushRelay>(
                ZLinkEnvelopeCodec.DecodeBody(
                    parts,
                    typeof(ZLinkRemoteSessionPushRelay)));
            RecordRemoteSessionPushSubmission();
        }
    }

    internal async ValueTask RouteSessionAsync(
        IZLinkBackendSessionRelocationBarrier transport,
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
        CancellationToken cancellationToken)
    {
        SessionRoute = route;
        SessionRouteStarted.TrySetResult();
        await transport.RouteSessionRelocationAsync(
                targetNodeRid,
                route,
                cancellationToken)
            .ConfigureAwait(false);
        SessionRouteCompleted.TrySetResult();
    }

    internal async ValueTask DestroyActorAsync(
        IZLinkBackendSpotNode transport,
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var attempt = Interlocked.Increment(ref _targetRollbackDestroyCount);
        TargetRollbackDestroyStarted.TrySetResult();
        if (attempt == 2)
            RetriedTargetRollbackDestroyStarted.TrySetResult();
        await (attempt == 2 && _holdRetriedTargetRollbackDestroy
                ? ReleaseRetriedTargetRollbackDestroy.Task
                : ReleaseTargetRollbackDestroy.Task)
            .WaitAsync(cancellationToken);
        await transport.DestroyActorAsync(actor, timeout, cancellationToken)
            .ConfigureAwait(false);
        TargetRollbackDestroyCompleted.TrySetResult();
        if (attempt == 2)
            RetriedTargetRollbackDestroyCompleted.TrySetResult();
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed class ReadyGatedCanonicalRelocationTarget(
        ICanonicalRelocationTarget inner,
        CanonicalRelocationTransportProbe probe) : ICanonicalRelocationTarget
    {
        public async ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord>
            PrepareAsync(
                ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
                ZLinkRelocationEnvelope envelope,
                RoutingId authenticatedSourceNodeRid,
                ZLinkCanonicalRelocationPreparationLease lease,
                CancellationToken cancellationToken)
        {
            var call = Interlocked.Increment(ref probe._targetPrepareCallCount);
            probe._prepareSourceNodeRid = authenticatedSourceNodeRid;
            ZLinkServiceWireCodec.RelocationReadyRecord ready;
            try
            {
                ready = await inner.PrepareAsync(
                        prepare,
                        envelope,
                        authenticatedSourceNodeRid,
                        lease,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (ZLinkFrameworkException exception)
                when (call > 1
                      && exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
            {
                probe.DuplicateTargetPrepareRejected.TrySetResult();
                throw;
            }
            if (Interlocked.Exchange(ref probe._failTargetReadyOnce, 0) != 0)
            {
                probe.TargetReadyFailureInjected.TrySetResult();
                throw new IOException("Injected READY terminal failure.");
            }
            probe.TargetPreparedBeforeReadySend.TrySetResult();
            var readyTerminal = await Task.WhenAny(
                    probe.ReleaseTargetReadySend.Task,
                    probe.FailHeldTargetReadySend.Task)
                .WaitAsync(cancellationToken);
            if (ReferenceEquals(readyTerminal, probe.FailHeldTargetReadySend.Task))
                throw new IOException("Injected held READY terminal failure.");
            return ready;
        }

        public void ReadySubmitted(
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            RoutingId authenticatedSourceNodeRid) =>
            inner.ReadySubmitted(prepare, authenticatedSourceNodeRid);

        public void ReadySubmissionFailed(
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            RoutingId authenticatedSourceNodeRid) =>
            inner.ReadySubmissionFailed(prepare, authenticatedSourceNodeRid);

        public ValueTask AbortPreparedAsync(
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            RoutingId authenticatedSourceNodeRid) =>
            AbortAsync(prepare, authenticatedSourceNodeRid);

        private async ValueTask AbortAsync(
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            RoutingId authenticatedSourceNodeRid)
        {
            var call = Interlocked.Increment(ref probe._targetAbortCallCount);
            if (call == 1)
                probe.TargetAbortCallStarted.TrySetResult();
            if (call == 1 && probe._holdTargetAbortRegistration)
                await probe.ReleaseTargetAbortRegistration.Task;
            await inner.AbortPreparedAsync(
                    prepare,
                    authenticatedSourceNodeRid)
                .ConfigureAwait(false);
            probe.TargetPrepareAborted.TrySetResult();
        }

        public ValueTask StageDataAsync(
            ZLinkServiceWireCodec.RelocationDataRecord data,
            RoutingId authenticatedSourceNodeRid,
            CancellationToken cancellationToken) =>
            inner.StageDataAsync(
                data,
                authenticatedSourceNodeRid,
                cancellationToken);

        public ValueTask CutoverAsync(
            ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
            RoutingId authenticatedSourceNodeRid,
            CancellationToken cancellationToken) =>
            inner.CutoverAsync(
                cutover,
                authenticatedSourceNodeRid,
                cancellationToken);
    }
}

internal sealed class ProbedBackendAdapterFactory(
    IZLinkBackendAdapterFactory inner,
    CanonicalRelocationTransportProbe probe) : IZLinkBackendAdapterFactory
{
    public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
        new ProbedBackendRuntimeContext(inner.CreateRuntimeContext(), probe);

    public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() =>
        inner.CreateMonitoringAdapter();
}

internal sealed class ProbedBackendRuntimeContext(
    IZLinkBackendRuntimeContext inner,
    CanonicalRelocationTransportProbe probe) : IZLinkBackendRuntimeContext
{
    private IZLinkBackendSpotNode? _innerSpotNode;
    private IZLinkBackendSpotNode? _probedSpotNode;

    public void ConfigureCoreHwm(
        AutoHwmProfile profile,
        ulong memoryLimitBytes,
        ulong budgetBytes) =>
        inner.ConfigureCoreHwm(profile, memoryLimitBytes, budgetBytes);

    public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() =>
        inner.GetCoreHwmBudgetSnapshot();

    public void ResetCoreHwmBudgetMetrics() =>
        inner.ResetCoreHwmBudgetMetrics();

    public void ConfigureApplicationJobQueue(
        Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobQueue applicationJobQueue) =>
        inner.ConfigureApplicationJobQueue(applicationJobQueue);

    public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();
    public IRouterSocket CreateRouterSocket() => inner.CreateRouterSocket();
    public IPubSocket CreatePublisherSocket() => inner.CreatePublisherSocket();
    public ISubSocket CreateSubscriberSocket() => inner.CreateSubscriberSocket();

    public IZLinkBackendSpotNode CreateSpotNode(string meshName)
    {
        _innerSpotNode = inner.CreateSpotNode(meshName);
        _probedSpotNode = ProbedBackendSpotNode.Create(_innerSpotNode, probe);
        return _probedSpotNode;
    }

    public IZLinkBackendStreamSocket CreateStreamSocket(
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null) =>
        inner.CreateStreamSocket(
            standaloneMeshName,
            ReferenceEquals(actorDispatchNode, _probedSpotNode)
                ? _innerSpotNode
                : actorDispatchNode);

    public ValueTask DisposeAsync() => inner.DisposeAsync();
}

internal interface IProbedBackendSpotNode :
    IZLinkBackendSpotNode,
    IZLinkBackendRelocationReplyRelay,
    IZLinkBackendCanonicalRelocation,
    IZLinkBackendSessionRelocationBarrier,
    IZLinkBackendAuthorityObserver,
    IZLinkBackendRequestSourceFenceObserver,
    IZLinkBackendLocalActorAuthorityReader,
    IZLinkBackendActorMessageFollowIngress,
    IZLinkBackendMessageFollowNotifications,
    IZLinkBackendBoundSessionReplacementNotifications;

internal class ProbedBackendSpotNode : DispatchProxy
{
    private object _target = null!;
    private CanonicalRelocationTransportProbe _probe = null!;

    internal static IZLinkBackendSpotNode Create(
        IZLinkBackendSpotNode target,
        CanonicalRelocationTransportProbe probe)
    {
        var proxy = Create<IProbedBackendSpotNode, ProbedBackendSpotNode>();
        var state = (ProbedBackendSpotNode)(object)proxy;
        state._target = target;
        state._probe = probe;
        return proxy;
    }

    protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
    {
        ArgumentNullException.ThrowIfNull(targetMethod);
        args ??= [];
        var canonical = (IZLinkBackendCanonicalRelocation)_target;
        if (targetMethod.Name == nameof(
                IZLinkBackendCanonicalRelocation.SetCanonicalRelocationTarget))
        {
            canonical.SetCanonicalRelocationTarget(
                _probe.WrapTarget((ICanonicalRelocationTarget)args[0]!));
            return null;
        }
        if (targetMethod.Name == nameof(
                IZLinkBackendCanonicalRelocation.PrepareCanonicalRelocationAsync))
            return _probe.PrepareAsync(
                canonical,
                (RoutingId)args[0]!,
                (ZLinkServiceWireCodec.RelocationPrepareRecord)args[1]!,
                (ZLinkRelocationTransferPayload)args[2]!,
                (TimeSpan)args[3]!,
                (CancellationToken)args[4]!);
        if (targetMethod.Name == nameof(
                IZLinkBackendCanonicalRelocation.SendCanonicalRelocationDataAsync))
            return _probe.SendDataAsync(
                canonical,
                (RoutingId)args[0]!,
                (ZLinkServiceWireCodec.RelocationDataRecord)args[1]!,
                (CancellationToken)args[2]!);
        if (targetMethod.Name == nameof(
                IZLinkBackendCanonicalRelocation.SendCanonicalRelocationCutoverAsync))
            return _probe.SendCutoverAsync(
                canonical,
                (RoutingId)args[0]!,
                (ZLinkServiceWireCodec.RelocationCutoverRecord)args[1]!,
                (CancellationToken)args[2]!);
        if (targetMethod.Name == nameof(IZLinkBackendSpotNode.SendToNodeAsync))
            return _probe.SendNodeAsync(
                (IZLinkBackendSpotNode)_target,
                (RoutingId)args[0]!,
                (IReadOnlyList<Message>)args[1]!,
                (SendFlags)args[2]!,
                (CancellationToken)args[3]!,
                (ReadOnlyMemory<byte>)args[4]!);
        if (targetMethod.Name == nameof(
                IZLinkBackendSessionRelocationBarrier.RouteSessionRelocationAsync))
            return _probe.RouteSessionAsync(
                (IZLinkBackendSessionRelocationBarrier)_target,
                (RoutingId)args[0]!,
                (ZLinkServiceWireCodec.SessionRelocationRouteRecord)args[1]!,
                (CancellationToken)args[2]!);
        if (targetMethod.Name == nameof(IZLinkBackendSpotNode.DestroyActorAsync))
            return _probe.DestroyActorAsync(
                (IZLinkBackendSpotNode)_target,
                (ZLinkBackendActorRef)args[0]!,
                (TimeSpan)args[1]!,
                (CancellationToken)args[2]!);

        try
        {
            var result = targetMethod.Invoke(_target, args);
            if (targetMethod.Name == nameof(IZLinkBackendSpotNode.SendToNode)
                && result is SubmitResult.Ok
                && args[1] is IReadOnlyList<Message> parts)
            {
                var packetName = ZLinkEnvelopeCodec.DecodeHeader(parts).MessageName;
                if (packetName == ZLinkRemoteActorSourceLeaveProtocol.PacketName)
                    _probe.RecordSourceLeaveSubmission();
                else if (packetName == ZLinkRemoteSessionPushProtocol.PacketName)
                    _probe.RecordRemoteSessionPushSubmission();
            }
            return result;
        }
        catch (TargetInvocationException error) when (error.InnerException is { } inner)
        {
            ExceptionDispatchInfo.Capture(inner).Throw();
            throw;
        }
    }
}

internal sealed class RelocationBehaviorSessionHandlers
    : IZLinkSessionHandlerRegistry
{
    public void AddHandler<THandler>() where THandler : class
    {
    }

    public void AddHandler<THandler>(string packetName) where THandler : class
    {
    }

    public ValueTask<bool> TryHandleAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(false);
}

internal sealed class RelocationBehaviorStream(RoutingId routingId) : IZLinkStream
{
    private int _writeCount;

    internal int WriteCount => Volatile.Read(ref _writeCount);

    internal TaskCompletionSource FirstWrite { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public string SessionId { get; } = routingId.ToHex();

    public RoutingId? RoutingId { get; } = routingId;

    public string? LocalAddr => null;

    public string? RemoteAddr => null;

    public bool Write(
        ZLinkMessage payload,
        SendFlags flags = SendFlags.None)
    {
        Interlocked.Increment(ref _writeCount);
        FirstWrite.TrySetResult();
        return true;
    }

    public ValueTask CloseAsync() => ValueTask.CompletedTask;
}

internal static class RelocationBehaviorListExtensions
{
    internal static int IndexOf(this IReadOnlyList<string> values, string value)
    {
        for (var index = 0; index < values.Count; index++)
            if (string.Equals(values[index], value, StringComparison.Ordinal))
                return index;
        return -1;
    }
}
