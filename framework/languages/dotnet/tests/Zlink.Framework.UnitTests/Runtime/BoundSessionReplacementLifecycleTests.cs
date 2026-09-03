using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class BoundSessionReplacementLifecycleTests
{
    [Fact]
    public async Task Replacement_Closes_Application_Ingress_Allows_Guidance_And_Deduplicates_Callback()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.Success);
        fixture.BindRetiredSessionActor("actor-a");

        Assert.True(fixture.NotifyReplacement("actor-a"));
        Assert.True(fixture.NotifyReplacement("actor-a"));

        var admissions = await Task.WhenAll(
            Enumerable.Range(0, 64).Select(_ => Task.Run(() =>
            {
                using var header = Message.From([1]);
                using var payload = Message.From([2]);
                return fixture.Session.TryEnqueuePacket(header, payload);
            })));
        Assert.All(admissions, static admission =>
            Assert.Equal(ZLinkSerialPostAdmission.Closed, admission));
        await fixture.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Socket.FrameSent.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Time.TimerScheduled.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await WaitUntilAsync(() => fixture.Time.ActiveTimerCount == 1);

        Assert.Equal(1, fixture.Lifetime.CallbackCount);
        Assert.Equal(1, fixture.Socket.SendCount);
        Assert.Equal(0, fixture.Socket.DisconnectCount);
        fixture.Time.Advance(TimeSpan.FromMilliseconds(99));
        Assert.Equal(0, fixture.Socket.DisconnectCount);
        fixture.Time.Advance(TimeSpan.FromMilliseconds(1));
        await fixture.Socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, fixture.Socket.DisconnectCount);
    }

    [Fact]
    public async Task Callback_OperationCanceledException_Is_A_Failure_With_The_Fixed_Grace_Window()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.ThrowOperationCanceled);
        fixture.BindRetiredSessionActor("actor-a");

        Assert.True(fixture.NotifyReplacement("actor-a"));
        await fixture.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Time.TimerScheduled.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await WaitUntilAsync(() => fixture.Time.ActiveTimerCount == 1);

        Assert.Equal(0, fixture.Socket.DisconnectCount);
        fixture.Time.Advance(TimeSpan.FromMilliseconds(99));
        Assert.Equal(0, fixture.Socket.DisconnectCount);
        fixture.Time.Advance(TimeSpan.FromMilliseconds(1));
        await fixture.Socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, fixture.Socket.DisconnectCount);
    }

    [Fact]
    public async Task Callback_Deadline_Force_Closes_Without_Starting_The_Grace_Timer()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.WaitForDeadline,
            sessionReplacementCallbackTimeout: TimeSpan.FromMilliseconds(40));
        fixture.BindRetiredSessionActor("actor-a");

        Assert.True(fixture.NotifyReplacement("actor-a"));
        await fixture.Lifetime.CallbackStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(1, fixture.Socket.DisconnectCount);
        Assert.Equal(0, fixture.Time.ActiveTimerCount);
    }

    [Fact]
    public async Task Grace_Timer_Revalidates_The_Exact_Retired_Binding_Before_Close()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.Success);
        var bindingToken = fixture.BindRetiredSessionActor("actor-a");

        Assert.True(fixture.NotifyReplacement("actor-a"));
        await fixture.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Time.TimerScheduled.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await WaitUntilAsync(() => fixture.Time.ActiveTimerCount == 1);
        fixture.Runtime.UnbindSessionActor(
            "actor-a",
            fixture.Lifetime.Context,
            bindingToken);

        fixture.Time.Advance(TimeSpan.FromMilliseconds(100));
        await Task.Yield();
        Assert.Equal(0, fixture.Socket.DisconnectCount);
    }

    [Fact]
    public async Task Physical_Close_Cleans_All_Retired_Actors_And_Preserves_The_Replacement()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.Success);
        fixture.BindRetiredSessionActor("actor-a");
        fixture.BindRetiredSessionActor("actor-b");
        var newSessionRid = RoutingId.From("replacement-session");
        var replacementToken = ZLinkActorBoundSessionBindingToken.Native(newSessionRid);
        var oldToken = ZLinkActorBoundSessionBindingToken.Native(fixture.SessionRid);
        fixture.Runtime.BindActorSession(
            "actor-a",
            null,
            newSessionRid,
            replacementToken,
            objectGeneration: 1,
            authorityOwnerGeneration: 2,
            meshName: "actors",
            ownerLeaseGeneration: 2);
        fixture.Runtime.BindActorSession(
            "actor-b",
            null,
            fixture.SessionRid,
            oldToken,
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            meshName: "actors",
            ownerLeaseGeneration: 1);

        Assert.True(fixture.NotifyReplacement("actor-a"));
        await fixture.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Time.TimerScheduled.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await WaitUntilAsync(() => fixture.Time.ActiveTimerCount == 1);
        fixture.Time.Advance(TimeSpan.FromMilliseconds(100));
        await fixture.Socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await fixture.Session.DisposeAsync();

        Assert.True(fixture.Runtime.TryGetActorBoundSession("actor-a", out var replacement));
        Assert.Equal(newSessionRid, replacement.SessionRid);
        Assert.Equal(replacementToken, replacement.BindingToken);
        Assert.False(fixture.Runtime.TryGetActorBoundSession("actor-b", out _));
        Assert.False(fixture.Runtime.TryGetSessionActorBinding("actor-a", out _));
        Assert.False(fixture.Runtime.TryGetSessionActorBinding("actor-b", out _));
    }

    [Fact]
    public async Task Replacement_Transport_Reaches_Runtime_And_Requires_Exact_Owner_Fence()
    {
        var suffix = Guid.NewGuid().ToString("N");
        var targetRid = RoutingId.From($"replacement-target-{suffix}");
        var sourceRid = RoutingId.From($"replacement-source-{suffix}");
        var targetEndpoint = "tcp://127.0.0.1:0";
        var sourceEndpoint = "tcp://127.0.0.1:0";
        await using var target = await HostedReplacementFixture.CreateAsync(
            targetRid,
            targetEndpoint,
            ReplacementCallbackBehavior.Success);
        await using var source = await HostedReplacementFixture.CreateAsync(
            sourceRid,
            sourceEndpoint,
            ReplacementCallbackBehavior.Success);

        source.Node.ConnectPeer(targetRid, target.Endpoint);
        await WaitUntilAsync(() =>
            target.Node.MeshStatus().AdmittedPeerCount == 1
            && source.Node.MeshStatus().AdmittedPeerCount == 1);

        target.BindRetiredSessionActor("actor-a");
        var targetGeneration = target.Node.MeshStatus().LifecycleGeneration;
        var sourceGeneration = target.Node.MeshPeers()
            .Single(peer => peer.RoutingId == sourceRid)
            .LifecycleGeneration;
        var sourceSender = Assert.IsAssignableFrom<
            IZLinkBackendBoundSessionReplacementNotifications>(source.Node);

        ZLinkServiceWireCodec.BoundSessionReplacedRecord Record(
            string actorId = "actor-a",
            string sessionOwnerId = "",
            ulong? sessionOwnerLeaseGeneration = null,
            RoutingId? sessionRid = null,
            ulong? retiredBindingGeneration = null,
            RoutingId? authoritySourceNodeRid = null,
            ulong? authoritySourceNodeGeneration = null,
            ulong? sessionOwnerNodeGeneration = null) => new(
            new ZLinkServiceWireCodec.BoundSessionReplacedActorAuthority(
                actorId,
                ObjectGeneration: 1,
                authoritySourceNodeRid ?? sourceRid,
                authoritySourceNodeGeneration ?? sourceGeneration,
                ExpectedAuthorityOwnerGeneration: 3,
                ExpectedOwnerLeaseGeneration: 5),
            new ZLinkServiceWireCodec.BoundSessionReplacedRetiredSession(
                targetRid,
                sessionOwnerNodeGeneration ?? targetGeneration,
                string.IsNullOrEmpty(sessionOwnerId)
                    ? target.SessionOwnerId
                    : sessionOwnerId,
                sessionOwnerLeaseGeneration ?? target.SessionOwnerLeaseGeneration,
                sessionRid ?? target.SessionRid,
                retiredBindingGeneration ?? target.BindingGeneration));

        Assert.True(sourceSender.TrySendBoundSessionReplacedNotification(
            targetRid,
            Record()));
        await target.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, Volatile.Read(ref target.Lifetime.CallbackCount));
        Assert.Equal(0, target.Socket.DisconnectCount);

        var invalidRecords = new[]
        {
            Record(authoritySourceNodeRid: RoutingId.From("forged-source")),
            Record(authoritySourceNodeGeneration: sourceGeneration + 1),
            Record(sessionOwnerNodeGeneration: targetGeneration + 1),
            Record(sessionOwnerId: "forged-owner"),
            Record(sessionOwnerLeaseGeneration: target.SessionOwnerLeaseGeneration + 1),
            Record(sessionRid: RoutingId.From("forged-session")),
            Record(retiredBindingGeneration: target.BindingGeneration + 1)
        };
        foreach (var invalid in invalidRecords)
        {
            Assert.True(sourceSender.TrySendBoundSessionReplacedNotification(
                targetRid,
                invalid));
        }

        target.BindRetiredSessionActor(
            "actor-b",
            target.BindingGeneration + 1);
        Assert.True(sourceSender.TrySendBoundSessionReplacedNotification(
            targetRid,
            Record(
                actorId: "actor-b",
                retiredBindingGeneration: target.BindingGeneration + 1)));
        await target.Lifetime.ActorBCallback.Task.WaitAsync(TimeSpan.FromSeconds(2));

        // A third valid record is a serial transport/dispatch barrier.  Once
        // actor-c has entered the session executor, all earlier records from
        // this sender have been received and enqueued in order.
        target.BindRetiredSessionActor(
            "actor-c",
            target.BindingGeneration + 2);
        Assert.True(sourceSender.TrySendBoundSessionReplacedNotification(
            targetRid,
            Record(
                actorId: "actor-c",
                retiredBindingGeneration: target.BindingGeneration + 2)));
        await target.Lifetime.ActorCCallback.Task.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(
            new[] { "actor-a", "actor-b", "actor-c" },
            target.Lifetime.CallbackActorIds.ToArray());
        Assert.Equal(3, Volatile.Read(ref target.Lifetime.CallbackCount));
        Assert.Equal(0, target.Socket.DisconnectCount);
    }

    [Fact]
    public async Task Retired_Binding_Lookup_Requires_Every_Owner_Fence_Field()
    {
        await using var fixture = await ReplacementFixture.CreateAsync(
            ReplacementCallbackBehavior.Success);
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromSeconds(30),
            new ZLinkLocationOptions().SessionRelocationSealTimeout);
        const string actorId = "actor-fence";
        const string bindingToken = "binding-fence";
        var actor = new ActorRef(
            actorId,
            1,
            "actors",
            RoutingId.From("actor-node"));
        var bound = new ZLinkSessionActor(
            fixture.Lifetime.Context,
            actorId,
            fixture.SessionRid,
            bindingToken);
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        _ = await table.BindAsync(
            actorKey,
            fixture.Lifetime.Context,
            bindingToken,
            bound,
            ReplacementFixture.BindingGeneration,
            ZLinkSessionBindingRoute.Create(
                actor,
                "actors",
                targetNodeGeneration: 2,
                authorityOwnerGeneration: 3,
                ownerLeaseGeneration: 5),
            ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerNodeRid,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration);

        await AssertExactAsync(true, actorId, ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, "other-actor", ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, RoutingId.From("other-owner-node"),
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, ReplacementFixture.SessionOwnerNodeRid,
            RoutingId.From("other-session"), ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration + 1,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            "other-owner", ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration + 1,
            ReplacementFixture.BindingGeneration);
        await AssertExactAsync(false, actorId, ReplacementFixture.SessionOwnerNodeRid,
            fixture.SessionRid, ReplacementFixture.SessionOwnerNodeGeneration,
            ReplacementFixture.SessionOwnerId,
            ReplacementFixture.SessionOwnerLeaseGeneration,
            ReplacementFixture.BindingGeneration + 1);
        return;

        async Task AssertExactAsync(
            bool expected,
            string candidateActorId,
            RoutingId ownerNodeRid,
            RoutingId sessionRid,
            ulong ownerNodeGeneration,
            string ownerId,
            ulong ownerLeaseGeneration,
            ulong bindingGeneration)
        {
            Assert.Equal(
                expected,
                await table.GetExactRetiredBindingAsync(
                    candidateActorId,
                    ownerNodeRid,
                    sessionRid,
                    ownerNodeGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    bindingGeneration) is not null);
        }
    }

    [Fact]
    public async Task Notification_Admission_Retries_Asynchronously_Until_Accepted()
    {
        var time = new ReplacementTimeProvider();
        var attempts = 0;

        var retry = ZLinkBoundSessionReplacementAdmissionRetry.RunAsync(
            () => Interlocked.Increment(ref attempts) == 3,
            TimeSpan.FromSeconds(1),
            time,
            CancellationToken.None).AsTask();

        Assert.Equal(1, Volatile.Read(ref attempts));
        Assert.False(retry.IsCompleted);
        time.Advance(TimeSpan.FromMilliseconds(10));
        await WaitUntilAsync(() => Volatile.Read(ref attempts) == 2
                                   && time.ActiveTimerCount == 1);
        time.Advance(TimeSpan.FromMilliseconds(10));

        Assert.True(await retry.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(3, Volatile.Read(ref attempts));
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Fact]
    public async Task Notification_Admission_Stops_At_Its_Own_Deadline()
    {
        var time = new ReplacementTimeProvider();
        var attempts = 0;

        var retry = ZLinkBoundSessionReplacementAdmissionRetry.RunAsync(
            () =>
            {
                Interlocked.Increment(ref attempts);
                return false;
            },
            TimeSpan.FromMilliseconds(15),
            time,
            CancellationToken.None).AsTask();

        time.Advance(TimeSpan.FromMilliseconds(10));
        await WaitUntilAsync(() => Volatile.Read(ref attempts) == 2
                                   && time.ActiveTimerCount == 1);
        time.Advance(TimeSpan.FromMilliseconds(5));

        Assert.False(await retry.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(3, Volatile.Read(ref attempts));
        Assert.Equal(0, time.ActiveTimerCount);
    }

    private static async Task WaitUntilAsync(Func<bool> predicate)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (!predicate())
        {
            Assert.True(DateTime.UtcNow < deadline);
            await Task.Delay(1);
        }
    }

    private enum ReplacementCallbackBehavior
    {
        Success,
        ThrowOperationCanceled,
        WaitForDeadline
    }

    private sealed class ReplacementSession(
        IZLinkSessionContext context,
        ReplacementLifetime lifetime) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = lifetime.Capture(context);

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref lifetime.DisconnectedCount);
            lifetime.Disconnected.TrySetResult();
            return ValueTask.CompletedTask;
        }

        public async ValueTask OnActorBindingReplacedAsync(
            string actorId,
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref lifetime.CallbackCount);
            lifetime.CallbackActorIds.Enqueue(actorId);
            if (actorId == "actor-b")
                lifetime.ActorBCallback.TrySetResult();
            else if (actorId == "actor-c")
                lifetime.ActorCCallback.TrySetResult();
            lifetime.CallbackStarted.TrySetResult();
            if (lifetime.Behavior == ReplacementCallbackBehavior.WaitForDeadline)
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                return;
            }

            await Context.Client.Send(new ReplacementGuidance(actorId)).Async();
            lifetime.CallbackTerminal.TrySetResult();
            if (lifetime.Behavior == ReplacementCallbackBehavior.ThrowOperationCanceled)
                throw new OperationCanceledException("application callback failure");
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed record ReplacementGuidance(string ActorId);

    private sealed class ReplacementLifetime(
        ReplacementCallbackBehavior behavior)
    {
        public ReplacementCallbackBehavior Behavior { get; } = behavior;

        public ZLinkSessionContext Context { get; set; } = null!;

        internal IZLinkSessionContext Capture(IZLinkSessionContext context)
        {
            Context = Assert.IsType<ZLinkSessionContext>(context);
            return context;
        }

        public TaskCompletionSource CallbackStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CallbackTerminal { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ConcurrentQueue<string> CallbackActorIds { get; } = new();

        public TaskCompletionSource ActorBCallback { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ActorCCallback { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Disconnected { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int CallbackCount;
        public int DisconnectedCount;
    }

    private sealed class ReplacementFixture : IAsyncDisposable
    {
        internal const ulong BindingGeneration = 7;
        internal const ulong SessionOwnerNodeGeneration = 11;
        internal const string SessionOwnerId = "session-owner";
        internal const ulong SessionOwnerLeaseGeneration = 13;

        internal static readonly RoutingId SessionOwnerNodeRid =
            RoutingId.From("session-owner-node");

        private readonly ServiceProvider _provider;

        private ReplacementFixture(
            ServiceProvider provider,
            ZLinkFrameworkRuntime runtime,
            ReplacementTimeProvider time,
            ReplacementStreamSocket socket,
            ZLinkStreamSessionRuntime session,
            ReplacementLifetime lifetime,
            RoutingId sessionRid)
        {
            _provider = provider;
            Runtime = runtime;
            Time = time;
            Socket = socket;
            Session = session;
            Lifetime = lifetime;
            SessionRid = sessionRid;
        }

        internal ZLinkFrameworkRuntime Runtime { get; }
        internal ReplacementTimeProvider Time { get; }
        internal ReplacementStreamSocket Socket { get; }
        internal ZLinkStreamSessionRuntime Session { get; }
        internal ReplacementLifetime Lifetime { get; }
        internal RoutingId SessionRid { get; }

        internal static async Task<ReplacementFixture> CreateAsync(
            ReplacementCallbackBehavior behavior,
            TimeSpan? sessionReplacementCallbackTimeout = null)
        {
            var registration = new ZLinkFrameworkRegistration
            {
                DefaultRequestTimeout = TimeSpan.FromSeconds(5),
                SessionReplacementCallbackTimeout =
                    sessionReplacementCallbackTimeout ?? TimeSpan.FromSeconds(5)
            };
            var lifetime = new ReplacementLifetime(behavior);
            ZLinkFrameworkRuntime runtime = null!;
            var services = new ServiceCollection()
                .AddSingleton(registration)
                .AddSingleton(lifetime)
                .AddSingleton(_ => runtime);
            var provider = services.BuildServiceProvider();
            runtime = new ZLinkFrameworkRuntime(
                provider,
                null!,
                registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    provider.GetRequiredService<IServiceScopeFactory>(),
                    registration));
            var time = new ReplacementTimeProvider();
            var socket = new ReplacementStreamSocket();
            var sessionRid = RoutingId.From("retired-session");
            var session = await ZLinkStreamSessionRuntime.CreateAsync(
                provider,
                socket,
                sessionRid,
                typeof(ReplacementSession),
                static _ => { },
                "test",
                time);
            return new ReplacementFixture(
                provider,
                runtime,
                time,
                socket,
                session,
                lifetime,
                sessionRid);
        }

        internal string BindRetiredSessionActor(string actorId)
        {
            var bindingToken = $"binding-{actorId}";
            var actor = new ActorRef(
                actorId,
                1,
                "actors",
                RoutingId.From("actor-owner"));
            var bound = new ZLinkSessionActor(
                Lifetime.Context,
                actorId,
                SessionRid,
                bindingToken);
            _ = Runtime.BindSessionActor(
                actorId,
                Lifetime.Context,
                bindingToken,
                bound,
                BindingGeneration,
                ZLinkSessionBindingRoute.Create(
                    actor,
                    "actors",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 3,
                    ownerLeaseGeneration: 5),
                SessionOwnerNodeGeneration,
                SessionOwnerNodeRid,
                SessionOwnerId,
                SessionOwnerLeaseGeneration);
            return bindingToken;
        }

        internal bool NotifyReplacement(string actorId) =>
            Session.TryEnqueueActorBindingReplaced(
                actorId,
                SessionOwnerNodeRid,
                SessionOwnerNodeGeneration,
                SessionOwnerId,
                SessionOwnerLeaseGeneration,
                SessionRid,
                BindingGeneration,
                $"binding-{actorId}");

        public async ValueTask DisposeAsync()
        {
            await Session.DisposeAsync();
            await _provider.DisposeAsync();
        }
    }

    private sealed class HostedReplacementFixture : IAsyncDisposable
    {
        private readonly ServiceProvider _provider;

        private HostedReplacementFixture(
            ServiceProvider provider,
            ZLinkFrameworkRuntime runtime,
            IZLinkBackendSpotNode node,
            string endpoint,
            ReplacementTimeProvider time,
            ReplacementStreamSocket socket,
            ZLinkStreamSessionRuntime session,
            ReplacementLifetime lifetime,
            RoutingId sessionRid,
            ulong bindingGeneration)
        {
            _provider = provider;
            Runtime = runtime;
            Node = node;
            Endpoint = node.Status().LocalEndpoint;
            Time = time;
            Socket = socket;
            Session = session;
            Lifetime = lifetime;
            SessionRid = sessionRid;
            BindingGeneration = bindingGeneration;
            SessionOwnerNodeRid = node.RoutingId;
            SessionOwnerNodeGeneration = node.MeshStatus().LifecycleGeneration;
            SessionOwnerId = node.RoutingId.ToHex();
            SessionOwnerLeaseGeneration = SessionOwnerNodeGeneration;
        }

        internal ZLinkFrameworkRuntime Runtime { get; }
        internal IZLinkBackendSpotNode Node { get; }
        internal string Endpoint { get; }
        internal ReplacementTimeProvider Time { get; }
        internal ReplacementStreamSocket Socket { get; }
        internal ZLinkStreamSessionRuntime Session { get; }
        internal ReplacementLifetime Lifetime { get; }
        internal RoutingId SessionRid { get; }
        internal ulong BindingGeneration { get; }
        internal RoutingId SessionOwnerNodeRid { get; }
        internal ulong SessionOwnerNodeGeneration { get; }
        internal string SessionOwnerId { get; }
        internal ulong SessionOwnerLeaseGeneration { get; }

        internal static async Task<HostedReplacementFixture> CreateAsync(
            RoutingId nodeRid,
            string endpoint,
            ReplacementCallbackBehavior behavior)
        {
            var registration = new ZLinkFrameworkRegistration
            {
                DefaultRequestTimeout = TimeSpan.FromSeconds(5),
                ImplicitHandlerAutoRegistrationEnabled = false
            };
            registration.SpotNodes["actors"] = new ZLinkSpotNodeRegistration
            {
                SpotNodeName = "actors",
                RoutingId = nodeRid,
                Router = new ZLinkSpotRouterCapabilityRegistration
                {
                    BindEndpoint = endpoint
                }
            };

            var lifetime = new ReplacementLifetime(behavior);
            ZLinkFrameworkRuntime runtime = null!;
            var services = new ServiceCollection()
                .AddSingleton(registration)
                .AddSingleton(lifetime)
                .AddSingleton(_ => runtime)
                .BuildServiceProvider();
            runtime = new ZLinkFrameworkRuntime(
                services,
                new ZLinkDotNetBackendAdapterFactory(),
                registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    services.GetRequiredService<IServiceScopeFactory>(),
                    registration));
            await runtime.StartAsync(CancellationToken.None);

            var node = runtime.GetSpotNodeRuntime(nodeRid).Node;
            var time = new ReplacementTimeProvider();
            var socket = new ReplacementStreamSocket();
            var sessionRid = RoutingId.From(
                $"retired-session-{nodeRid.ToHex()}");
            var session = await ZLinkStreamSessionRuntime.CreateAsync(
                services,
                socket,
                sessionRid,
                typeof(ReplacementSession),
                static _ => { },
                "test",
                time);
            return new HostedReplacementFixture(
                services,
                runtime,
                node,
                endpoint,
                time,
                socket,
                session,
                lifetime,
                sessionRid,
                bindingGeneration: 7);
        }

        internal void BindRetiredSessionActor(
            string actorId,
            ulong? bindingGeneration = null)
        {
            var bindingToken = $"binding-{actorId}";
            var exactBindingGeneration = bindingGeneration ?? BindingGeneration;
            var actor = new ActorRef(
                actorId,
                1,
                "actors",
                RoutingId.From("actor-owner"));
            var bound = new ZLinkSessionActor(
                Lifetime.Context,
                actorId,
                SessionRid,
                bindingToken);
            _ = Runtime.BindSessionActor(
                actorId,
                Lifetime.Context,
                bindingToken,
                bound,
                exactBindingGeneration,
                ZLinkSessionBindingRoute.Create(
                    actor,
                    "actors",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 3,
                    ownerLeaseGeneration: 5),
                SessionOwnerNodeGeneration,
                SessionOwnerNodeRid,
                SessionOwnerId,
                SessionOwnerLeaseGeneration);
        }

        public async ValueTask DisposeAsync()
        {
            await Session.DisposeAsync();
            await Runtime.StopAsync(CancellationToken.None);
            await _provider.DisposeAsync();
        }
    }

    private sealed class ReplacementStreamSocket : IZLinkBackendStreamSocket
    {
        private int _disconnectCount;
        private int _sendCount;

        internal int DisconnectCount => Volatile.Read(ref _disconnectCount);
        internal int SendCount => Volatile.Read(ref _sendCount);

        internal TaskCompletionSource DisconnectStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal TaskCompletionSource FrameSent { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Bind(string endpoint) { }
        public void SetTlsServer(string certPath, string keyPath, bool requireClientCert) { }
        public IZLinkBackendSocketPoller CreateReceivePoller() =>
            throw new NotSupportedException();

        public bool RecvPacket(
            out ZLinkBackendStreamReceive? received,
            RecvFlags flags = RecvFlags.None)
        {
            received = null;
            return false;
        }

        public bool Send(RoutingId routingId, Message payload, SendFlags flags)
        {
            Interlocked.Increment(ref _sendCount);
            FrameSent.TrySetResult();
            return true;
        }

        public bool Send(
            RoutingId routingId,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            Interlocked.Increment(ref _sendCount);
            FrameSent.TrySetResult();
            return true;
        }

        public void DisconnectPeer(RoutingId routingId)
        {
            Interlocked.Increment(ref _disconnectCount);
            DisconnectStarted.TrySetResult();
        }

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags) => true;

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class ReplacementTimeProvider : TimeProvider
    {
        private readonly object _gate = new();
        private readonly HashSet<ReplacementTimer> _timers = [];
        private long _timestamp;

        internal TaskCompletionSource TimerScheduled { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal int ActiveTimerCount
        {
            get
            {
                lock (_gate) return _timers.Count(timer => timer.IsScheduled);
            }
        }

        public override long GetTimestamp() => Volatile.Read(ref _timestamp);

        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override ITimer CreateTimer(
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period)
        {
            var timer = new ReplacementTimer(this, callback, state);
            lock (_gate)
            {
                _timers.Add(timer);
                timer.ChangeCore(_timestamp, dueTime, period);
            }
            TimerScheduled.TrySetResult();
            return timer;
        }

        internal void Advance(TimeSpan delta)
        {
            if (delta < TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(nameof(delta));
            List<(TimerCallback Callback, object? State)> due = [];
            lock (_gate)
            {
                _timestamp = checked(_timestamp + delta.Ticks);
                foreach (var timer in _timers)
                {
                    if (timer.TryTakeDue(_timestamp, out var callback))
                        due.Add(callback);
                }
            }
            foreach (var callback in due)
                callback.Callback(callback.State);
        }

        private sealed class ReplacementTimer(
            ReplacementTimeProvider owner,
            TimerCallback callback,
            object? state) : ITimer
        {
            private long? _dueAt;
            private TimeSpan _period;
            private bool _disposed;

            internal bool IsScheduled => !_disposed && _dueAt.HasValue;

            public bool Change(TimeSpan dueTime, TimeSpan period)
            {
                lock (owner._gate)
                {
                    if (_disposed) return false;
                    ChangeCore(owner._timestamp, dueTime, period);
                    return true;
                }
            }

            internal void ChangeCore(long now, TimeSpan dueTime, TimeSpan period)
            {
                _period = period;
                _dueAt = dueTime == Timeout.InfiniteTimeSpan
                    ? null
                    : checked(now + dueTime.Ticks);
            }

            internal bool TryTakeDue(
                long now,
                out (TimerCallback Callback, object? State) due)
            {
                if (_disposed || _dueAt is not { } dueAt || dueAt > now)
                {
                    due = default;
                    return false;
                }
                _dueAt = _period > TimeSpan.Zero
                              && _period != Timeout.InfiniteTimeSpan
                    ? checked(now + _period.Ticks)
                    : null;
                due = (callback, state);
                return true;
            }

            public void Dispose()
            {
                lock (owner._gate)
                {
                    if (_disposed) return;
                    _disposed = true;
                    _dueAt = null;
                    owner._timers.Remove(this);
                }
            }

            public ValueTask DisposeAsync()
            {
                Dispose();
                return ValueTask.CompletedTask;
            }
        }
    }
}
