using Microsoft.Extensions.DependencyInjection;
using System.Diagnostics.Metrics;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

[Collection(RuntimeMetricsCollection.Name)]
public sealed class SessionActorCoordinatorTests
{
    [Fact]
    public async Task Session_Send_Submit_Reports_Nonblocking_Transport_Backpressure()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"), acceptsWrites: false);
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await context.Client.Send(new SessionPush("value")).Async());

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.Equal(SendFlags.DontWait, stream.LastWriteFlags);
    }

    [Fact]
    public async Task Session_Reply_PreCancellation_Claims_The_Reply_Token_Before_Admission()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        _ = context.EnterDispatch(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(1),
            "SessionRequest",
            ZlinkStreamMetadata.Empty));
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            context.Client.Reply(new SessionPush("cancelled"))
                .Async(cancellation.Token)
                .AsTask());
        Assert.Empty(stream.Writes);

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            context.Client.Reply(new SessionPush("duplicate"))
                .Async()
                .AsTask());
    }

    [Fact]
    public async Task BindActorAsync_Rebinds_Same_Ref_While_BindOrGet_Returns_Existing()
    {
        var runtime = CreateRuntime();
        var context = new ZLinkSessionContext(
            runtime,
            new TestStream(RoutingId.From("session-node")),
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            "actor-1",
            1,
            "actors",
            RoutingId.From("actor-node"));

        var first = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(
            actor.ActorId,
            out var firstBinding));

        var existing = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.Same(first, existing);
        Assert.True(runtime.TryGetSessionActorBinding(
            actor.ActorId,
            out var unchangedBinding));
        Assert.Equal(firstBinding.BindingToken, unchangedBinding.BindingToken);
        Assert.Equal(
            firstBinding.BindingGeneration,
            unchangedBinding.BindingGeneration);

        var rebound = await context.ActorCoordinator.BindActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.NotSame(first, rebound);
        Assert.True(runtime.TryGetSessionActorBinding(
            actor.ActorId,
            out var reboundBinding));
        Assert.NotEqual(firstBinding.BindingToken, reboundBinding.BindingToken);
        Assert.True(
            reboundBinding.BindingGeneration > firstBinding.BindingGeneration);
    }

    [Fact]
    public async Task BindOrGetActorAsync_Rebinds_When_Generation_Changes_For_Same_ActorId()
    {
        var runtime = CreateRuntime();
        var context = new ZLinkSessionContext(
            runtime,
            new TestStream(RoutingId.From("session-node")),
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);

        var firstRef = new ActorRef("actor-1", 1, "actors", RoutingId.From("actor-node"));
        var secondRef = new ActorRef("actor-1", 2, "actors", RoutingId.From("actor-node"));

        var first = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            firstRef,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding("actor-1", out var firstSession));
        var firstToken = firstSession.BindingToken;

        var second = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            secondRef,
            CancellationToken.None);

        Assert.NotSame(first, second);
        Assert.Equal(secondRef, second.Ref);
        Assert.Single(context.Actors.Bound);
        Assert.DoesNotContain(first, context.Actors.Bound);
        Assert.False(runtime.TryGetSessionActorContext("actor-1", firstToken, out _));
        Assert.True(runtime.TryGetSessionActorBinding("actor-1", out var secondSession));
        Assert.NotEqual(firstToken, secondSession.BindingToken);
        Assert.True(runtime.TryGetSessionActorContext("actor-1", secondSession.BindingToken, out var reboundContext));
        Assert.Same(context, reboundContext);
    }

    [Fact]
    public async Task Local_Actor_Bound_Session_Send_Uses_The_Bound_Stream()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef("actor-1", 1, "actors", RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(context, actor, CancellationToken.None);

        using var payload = Message.From(new byte[] { 1, 2, 3 });
        Assert.True(runtime.SendActorBoundSession(
            actor.ActorId,
            new[] { payload },
            SendFlags.DontWait));

        var frame = Assert.Single(stream.Writes);
        Assert.Equal(SendFlags.DontWait, frame.Flags);
        Assert.NotEmpty(frame.Payload);
    }

    [Fact]
    public async Task Remote_Actor_Reply_Uses_The_Preserved_Session_Request_Route()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            "actor-remote-reply",
            1,
            "actors",
            RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(
            actor.ActorId,
            out var binding));
        var replyCapability = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            requestId: 17,
            binding.BindingToken);
        var replyFrame = new byte[] { 4, 5, 6 };

        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            requestId: 17,
            flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replyCapability,
            sourceNodeRid: actor.NodeRid,
            responderNodeRid: actor.NodeRid,
            replyFrame,
            CancellationToken.None);

        var written = Assert.Single(stream.Writes);
        Assert.Equal(replyFrame, written.Payload);
        Assert.Equal(SendFlags.DontWait, written.Flags);
    }

    [Fact]
    public async Task Remote_Actor_Reply_Rejects_Unknown_Flags_And_Source()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            "actor-reply-fence",
            1,
            "actors",
            RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var binding));

        var wrongFlagsCapability = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            21,
            binding.BindingToken);
        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            21,
            flags: 0,
            wrongFlagsCapability,
            actor.NodeRid,
            actor.NodeRid,
            [1],
            CancellationToken.None);
        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            21,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            wrongFlagsCapability,
            actor.NodeRid,
            actor.NodeRid,
            [3],
            CancellationToken.None);

        var wrongSourceCapability = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            22,
            binding.BindingToken);
        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            22,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            wrongSourceCapability,
            RoutingId.From("forged-node"),
            actor.NodeRid,
            [2],
            CancellationToken.None);
        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            22,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            wrongSourceCapability,
            actor.NodeRid,
            actor.NodeRid,
            [4],
            CancellationToken.None);

        Assert.Equal(2, stream.Writes.Count);
        Assert.Equal(new byte[] { 3 }, stream.Writes[0].Payload);
        Assert.Equal(new byte[] { 4 }, stream.Writes[1].Payload);
    }

    [Fact]
    public async Task Concurrent_Duplicate_Remote_Replies_Write_One_Terminal_Frame()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-node"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            "actor-reply-once",
            1,
            "actors",
            RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var binding));
        Assert.True(runtime.TryAcceptSessionActorFrame(
            actor.ActorId,
            binding.BindingToken,
            out _));
        Assert.True(runtime.TryAcceptSessionActorFrame(
            actor.ActorId,
            binding.BindingToken,
            out _));
        var capability = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            24,
            binding.BindingToken);
        _ = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            26,
            binding.BindingToken);

        Task DeliverAsync() => runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            24,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            capability,
            actor.NodeRid,
            actor.NodeRid,
            [5],
            CancellationToken.None).AsTask();

        await Task.WhenAll(Task.Run(DeliverAsync), Task.Run(DeliverAsync));
        Assert.Single(stream.Writes);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var afterReply));
        Assert.Equal(1, afterReply.ActiveFrames);
        runtime.CompleteRemoteSessionActorRequest(
            actor.ActorId,
            binding.ObjectGeneration,
            binding.BindingToken,
            26);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var completed));
        Assert.Equal(0, completed.ActiveFrames);
    }

    [Fact]
    public async Task Expired_Request_Timer_Does_Not_Remove_A_Replacement_Correlation()
    {
        var runtime = CreateRuntime(defaultRequestTimeout: TimeSpan.FromMilliseconds(80));
        var stream = new TestStream(RoutingId.From("session-timeout"));
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            "actor-request-timeout",
            1,
            "actors",
            RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var binding));

        _ = runtime.TrackRemoteSessionActorRequest(actor.ActorId, 23, binding.BindingToken);
        runtime.CompleteRemoteSessionActorRequest(
            actor.ActorId,
            binding.ObjectGeneration,
            binding.BindingToken,
            23);
        await Task.Delay(40);
        var replacementCapability = runtime.TrackRemoteSessionActorRequest(
            actor.ActorId,
            23,
            binding.BindingToken);
        await Task.Delay(50);

        await runtime.DeliverRemoteActorReplyAsync(
            actor.ActorId,
            23,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replacementCapability,
            actor.NodeRid,
            actor.NodeRid,
            [6],
            CancellationToken.None);
        Assert.Single(stream.Writes);
    }

    [Fact]
    public async Task Rebind_Removes_The_Previous_Generation_Pending_Request()
    {
        var runtime = CreateRuntime();
        var previousStream = new TestStream(RoutingId.From("session-previous"));
        var previousContext = new ZLinkSessionContext(
            runtime,
            previousStream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var previousActor = new ActorRef(
            "actor-recreated",
            1,
            "actors",
            RoutingId.From("actor-node-old"));
        await previousContext.ActorCoordinator.BindOrGetActorAsync(
            previousContext,
            previousActor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(
            previousActor.ActorId,
            out var previousBinding));
        var previousCapability = runtime.TrackRemoteSessionActorRequest(
            previousActor.ActorId,
            requestId: 2,
            previousBinding.BindingToken);

        var replacementStream = new TestStream(RoutingId.From("session-replacement"));
        var replacementContext = new ZLinkSessionContext(
            runtime,
            replacementStream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var replacementActor = new ActorRef(
            previousActor.ActorId,
            2,
            "actors",
            RoutingId.From("actor-node-new"));
        await replacementContext.ActorCoordinator.BindOrGetActorAsync(
            replacementContext,
            replacementActor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(
            replacementActor.ActorId,
            out var replacementBinding));
        Assert.NotEqual(
            previousBinding.BindingToken,
            replacementBinding.BindingToken);
        Assert.Equal(2UL, replacementBinding.ObjectGeneration);

        var replacementCapability = runtime.TrackRemoteSessionActorRequest(
            replacementActor.ActorId,
            requestId: 2,
            replacementBinding.BindingToken);
        Assert.NotEqual(previousCapability, replacementCapability);

        await runtime.DeliverRemoteActorReplyAsync(
            previousActor.ActorId,
            requestId: 2,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            previousCapability,
            previousActor.NodeRid,
            previousActor.NodeRid,
            [1],
            CancellationToken.None);
        Assert.Empty(previousStream.Writes);
        Assert.Empty(replacementStream.Writes);

        await runtime.DeliverRemoteActorReplyAsync(
            replacementActor.ActorId,
            requestId: 2,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replacementCapability,
            replacementActor.NodeRid,
            replacementActor.NodeRid,
            [2],
            CancellationToken.None);
        Assert.Empty(previousStream.Writes);
        Assert.Single(replacementStream.Writes);
        Assert.Equal(new byte[] { 2 }, replacementStream.Writes[0].Payload);
    }

    [Fact]
    public async Task Remote_Actor_Request_Timeout_Releases_The_Pending_Key()
    {
        var runtime = CreateRuntime(defaultRequestTimeout: TimeSpan.FromMilliseconds(20));
        var context = CreateSessionContext(runtime, "session-timeout-release");
        var actor = new ActorRef(
            "actor-timeout-release",
            1,
            "actors",
            RoutingId.From("actor-node"));
        await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var binding));

        _ = runtime.TrackRemoteSessionActorRequest(actor.ActorId, 25, binding.BindingToken);
        await Task.Delay(80);
        _ = runtime.TrackRemoteSessionActorRequest(actor.ActorId, 25, binding.BindingToken);
        runtime.CompleteRemoteSessionActorRequest(
            actor.ActorId,
            binding.ObjectGeneration,
            binding.BindingToken,
            25);
    }

    [Fact]
    public void Stale_Local_Actor_Binding_Does_Not_Fall_Through_To_Native_Routing()
    {
        var runtime = CreateRuntime();
        runtime.BindActorSession(
            "actor-1",
            null,
            RoutingId.From("session-rid"),
            "local-binding-token",
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            meshName: "actors",
            ownerLeaseGeneration: 1);
        using var payload = Message.From(new byte[] { 1, 2, 3 });

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            runtime.SendActorBoundSession(
                "actor-1",
                new[] { payload },
                SendFlags.DontWait));

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, exception.Kind);
    }

    [Fact]
    public void Bound_Session_Cleanup_Is_Isolated_Per_Runtime_When_Session_Rids_Match()
    {
        var runtimeA = CreateRuntime();
        var runtimeB = CreateRuntime();
        var sharedSessionRid = RoutingId.From("shared-session");
        var token = ZLinkActorBoundSessionBindingToken.Native(sharedSessionRid);
        runtimeA.BindActorSession(
            "actor-a",
            null,
            sharedSessionRid,
            token,
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            meshName: "actors",
            ownerLeaseGeneration: 1);
        runtimeB.BindActorSession(
            "actor-b",
            null,
            sharedSessionRid,
            token,
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            meshName: "actors",
            ownerLeaseGeneration: 1);

        Assert.True(runtimeA.TryGetActorBoundSession("actor-a", out _));
        Assert.True(runtimeB.TryGetActorBoundSession("actor-b", out _));

        runtimeA.CleanupActorSessionsForSession(sharedSessionRid);

        Assert.False(runtimeA.TryGetActorBoundSession("actor-a", out _));
        Assert.True(runtimeB.TryGetActorBoundSession("actor-b", out var remaining));
        Assert.Equal(sharedSessionRid, remaining.SessionRid);
    }

    [Fact]
    public async Task Stale_Session_Disconnect_Does_Not_Notify_Or_Clear_The_Replacement_Binding()
    {
        var runtime = CreateRuntime();
        var firstContext = CreateSessionContext(runtime, "session-old");
        var replacementContext = CreateSessionContext(runtime, "session-new");
        var actor = new ActorRef("actor-1", 1, "actors", RoutingId.From("actor-node"));
        var oldBinding = await firstContext.ActorCoordinator.BindOrGetActorAsync(
            firstContext,
            actor,
            CancellationToken.None);
        _ = await replacementContext.ActorCoordinator.BindOrGetActorAsync(
            replacementContext,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var replacement));

        await oldBinding.NotifyDisconnectedAsync();

        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var current));
        Assert.Equal(replacement.BindingToken, current.BindingToken);
        Assert.True(runtime.TryGetSessionActorContext(
            actor.ActorId,
            current.BindingToken,
            out var currentContext));
        Assert.Same(replacementContext, currentContext);
    }

    [Fact]
    public void Remote_Disconnect_Removes_Only_The_Exact_Stored_Binding_Route()
    {
        var runtime = CreateRuntime();
        var actorId = "actor-remote-disconnect";
        var sessionNodeRid = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        runtime.BindActorSession(
            actorId,
            sessionNodeRid,
            sessionRid,
            "current-binding",
            bindingGeneration: 7,
            objectGeneration: 3,
            authorityOwnerGeneration: 5,
            meshName: "actors",
            targetNodeGeneration: 11,
            ownerLeaseGeneration: 13,
            sessionOwnerNodeGeneration: 17);
        var state = runtime.GetOrCreateActorState(actorId);
        Assert.True(runtime.TryGetActorBoundSession(actorId, out var current));
        using var stalePayload = Message.From(
            ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                "stale-binding",
                current.BindingGeneration,
                current.SessionOwnerNodeGeneration));

        Assert.False(ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
            state,
            sessionNodeRid,
            sessionRid,
            stalePayload,
            out _));
        Assert.True(runtime.TryGetActorBoundSession(actorId, out var retained));
        Assert.Equal("current-binding", retained.BindingToken);

        using var wrongGenerationPayload = Message.From(
            ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                current.BindingToken,
                current.BindingGeneration + 1,
                current.SessionOwnerNodeGeneration));
        Assert.False(ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
            state,
            sessionNodeRid,
            sessionRid,
            wrongGenerationPayload,
            out _));
        Assert.True(runtime.TryGetActorBoundSession(actorId, out _));

        using var emptyPayload = Message.From(Array.Empty<byte>());
        Assert.False(ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
            state,
            sessionNodeRid,
            sessionRid,
            emptyPayload,
            out _));

        using var exactPayload = Message.From(
            ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                current.BindingToken,
                current.BindingGeneration,
                current.SessionOwnerNodeGeneration));
        Assert.True(ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
            state,
            sessionNodeRid,
            sessionRid,
            exactPayload,
            out var exactBindingToken));
        Assert.True(runtime.TryGetActorBoundSession(actorId, out _));
        runtime.RemoveActorSessionBinding(actorId, exactBindingToken);
        Assert.False(runtime.TryGetActorBoundSession(actorId, out _));
    }

    [Fact]
    public async Task Rebind_Fences_Stale_Relay_And_Late_Disconnect_Without_Affecting_Other_Actors()
    {
        var runtime = CreateRuntime();
        var sessionA = CreateSessionContext(runtime, "session-a");
        var sessionB = CreateSessionContext(runtime, "session-b");
        var actorX = new ActorRef("actor-x", 7, "actors", RoutingId.From("actor-node"));
        var actorA = new ActorRef("actor-a", 1, "actors", RoutingId.From("actor-node"));
        var actorB = new ActorRef("actor-b", 1, "actors", RoutingId.From("actor-node"));
        var stale = await sessionA.ActorCoordinator.BindOrGetActorAsync(
            sessionA,
            actorX,
            CancellationToken.None);
        _ = await sessionA.ActorCoordinator.BindOrGetActorAsync(
            sessionA,
            actorA,
            CancellationToken.None);
        _ = await sessionB.ActorCoordinator.BindOrGetActorAsync(
            sessionB,
            actorB,
            CancellationToken.None);
        var current = await sessionB.ActorCoordinator.BindOrGetActorAsync(
            sessionB,
            actorX,
            CancellationToken.None);
        using var payload = Message.From(new byte[] { 1, 2, 3 });
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "ActorPing",
            ZlinkStreamMetadata.Empty);

        var staleRelay = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await sessionA.ActorCoordinator.RelayToActorAsync(
                stale,
                header,
                payload,
                static (_, _, _) => ValueTask.CompletedTask,
                CancellationToken.None));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, staleRelay.Kind);

        await stale.NotifyDisconnectedAsync();

        Assert.Null(sessionA.ActorCoordinator.FindActor(actorX.ActorId));
        Assert.Same(current, sessionB.ActorCoordinator.FindActor(actorX.ActorId));
        Assert.NotNull(sessionA.ActorCoordinator.FindActor(actorA.ActorId));
        Assert.NotNull(sessionB.ActorCoordinator.FindActor(actorB.ActorId));
        Assert.True(runtime.TryGetSessionActorBinding(actorX.ActorId, out var currentBinding));
        Assert.True(runtime.TryGetSessionActorContext(
            actorX.ActorId,
            currentBinding.BindingToken,
            out var currentContext));
        Assert.Same(sessionB, currentContext);
        Assert.Equal(actorX.ObjectGeneration, current.Ref.ObjectGeneration);
    }

    [Fact]
    public void Remote_Bound_Relay_Requires_The_Current_Session_Node_And_Session_Rid()
    {
        var currentNode = RoutingId.From("session-node-b");
        var currentSession = RoutingId.From("session-b");
        var binding = new ZLinkActorBoundSession(
            currentNode,
            currentSession,
            "binding-b",
            BindingGeneration: 2,
            ObjectGeneration: 7,
            AuthorityOwnerGeneration: 3,
            MeshName: "actors",
            TargetNodeGeneration: 4,
            OwnerLeaseGeneration: 5,
            SessionOwnerNodeGeneration: 6,
            AcceptedHighWater: 0);

        Assert.True(ZLinkActorBoundSessionRelay.MatchesRelaySource(
            binding,
            currentNode,
            currentSession));
        Assert.False(ZLinkActorBoundSessionRelay.MatchesRelaySource(
            binding,
            RoutingId.From("session-node-a"),
            currentSession));
        Assert.False(ZLinkActorBoundSessionRelay.MatchesRelaySource(
            binding,
            currentNode,
            RoutingId.From("session-a")));
    }

    [Fact]
    public async Task Physical_Disconnect_Uses_A_Fixed_AllSettled_Snapshot_And_Cleans_Every_Binding()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-physical-disconnect");
        var actorA = new ActorRef("actor-a", 3, "actors", RoutingId.From("actor-node"));
        var actorB = new ActorRef("actor-b", 5, "actors", RoutingId.From("actor-node"));
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actorA,
            CancellationToken.None);
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actorB,
            CancellationToken.None);
        var actorAState = runtime.GetOrCreateActorState(actorA.ActorId);
        var actorBState = runtime.GetOrCreateActorState(actorB.ActorId);

        await context.ActorCoordinator.CleanupAsync(context, CancellationToken.None);

        Assert.Empty(context.Actors.Bound);
        Assert.False(runtime.TryGetSessionActorBinding(actorA.ActorId, out _));
        Assert.False(runtime.TryGetSessionActorBinding(actorB.ActorId, out _));
        Assert.Same(actorAState, runtime.GetOrCreateActorState(actorA.ActorId));
        Assert.Same(actorBState, runtime.GetOrCreateActorState(actorB.ActorId));
    }

    [Fact]
    public async Task New_Object_Generation_Requires_An_Explicit_Bind()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-generation-fence");
        var generationOne = new ActorRef(
            "actor-generation",
            1,
            "actors",
            RoutingId.From("actor-node-a"));
        var generationTwo = new ActorRef(
            "actor-generation",
            2,
            "actors",
            RoutingId.From("actor-node-b"));
        var original = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            generationOne,
            CancellationToken.None);

        var explicitReplacement = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            generationTwo,
            CancellationToken.None);

        Assert.NotSame(original, explicitReplacement);
        Assert.Equal((ulong)2, explicitReplacement.Ref.ObjectGeneration);
        Assert.Null(context.Actors.Bound.SingleOrDefault(actor =>
            ReferenceEquals(actor, original)));
        Assert.Same(
            explicitReplacement,
            Assert.Single(context.Actors.Bound));
    }

    [Fact]
    public async Task Failed_Exact_Rebind_Preserves_The_Previous_Terminal_Binding()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-rebind-rollback");
        var previous = new ActorRef(
            "actor-rebind",
            1,
            "actors",
            RoutingId.From("actor-node-a"));
        var replacement = new ActorRef(
            previous.ActorId,
            2,
            "actors",
            RoutingId.From("actor-node-b"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            previous,
            CancellationToken.None);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            context.ActorCoordinator.BindOrGetActorAsync(
                    context,
                    replacement,
                    cancellation.Token)
                .AsTask());

        Assert.Same(bound, Assert.Single(context.Actors.Bound));
        Assert.Equal(previous, bound.Ref);
        Assert.True(runtime.TryGetSessionActorBinding(previous.ActorId, out var retained));
        Assert.Equal(previous.ObjectGeneration, retained.ObjectGeneration);
    }

    [Fact]
    public async Task Concurrent_Find_Never_Observes_A_Release_To_Bind_Gap()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-rebind-reader");
        var nodeRid = RoutingId.From("actor-node");
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            new ActorRef("actor-rebind-reader", 1, "actors", nodeRid),
            CancellationToken.None);
        var replacing = true;
        var reader = Task.Run(() =>
        {
            while (Volatile.Read(ref replacing))
                Assert.NotNull(context.ActorCoordinator.FindActor(
                    "actor-rebind-reader"));
        });

        for (ulong generation = 2; generation <= 64; generation++)
            _ = await context.ActorCoordinator.BindOrGetActorAsync(
                context,
                new ActorRef(
                    "actor-rebind-reader",
                    generation,
                    "actors",
                    nodeRid),
                CancellationToken.None);
        Volatile.Write(ref replacing, false);
        await reader;

        Assert.Equal(
            (ulong)64,
            Assert.Single(context.Actors.Bound).Ref.ObjectGeneration);
    }

    [Fact]
    public async Task Exact_Rebind_Replaces_A_Remote_Route_With_The_Local_Node_Route()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-local-node");
        var remote = new ActorRef(
            "actor-remote-to-local",
            1,
            "actors",
            RoutingId.From("remote-node"));
        var local = new ActorRef(
            remote.ActorId,
            2,
            "actors",
            RoutingId.From("session-local-node"));
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            remote,
            CancellationToken.None);

        var rebound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            local,
            CancellationToken.None);

        Assert.Equal(local, rebound.Ref);
        Assert.Equal(local, Assert.Single(context.Actors.Bound).Ref);
        Assert.True(runtime.TryGetSessionActorBinding(local.ActorId, out var exact));
        Assert.Equal(local.ObjectGeneration, exact.ObjectGeneration);
        Assert.Equal(local.MeshName, exact.MeshName);
    }

    [Fact]
    public async Task Concurrent_Exact_Replacements_Are_Serialized_Per_Actor()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-concurrent-rebind");
        var original = new ActorRef(
            "actor-concurrent-rebind",
            1,
            "actors",
            RoutingId.From("node-o"));
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            original,
            CancellationToken.None);
        var candidateA = new ActorRef(
            original.ActorId,
            2,
            "actors",
            RoutingId.From("node-a"));
        var candidateB = new ActorRef(
            original.ActorId,
            3,
            "actors",
            RoutingId.From("node-b"));
        using var start = new ManualResetEventSlim();

        var replacementA = Task.Run(async () =>
        {
            start.Wait();
            return await context.ActorCoordinator.BindOrGetActorAsync(
                context,
                candidateA,
                CancellationToken.None);
        });
        var replacementB = Task.Run(async () =>
        {
            start.Wait();
            return await context.ActorCoordinator.BindOrGetActorAsync(
                context,
                candidateB,
                CancellationToken.None);
        });
        start.Set();
        await Task.WhenAll(replacementA, replacementB);

        var terminal = Assert.Single(context.Actors.Bound);
        Assert.Contains(
            terminal.Ref,
            new[] { candidateA, candidateB });
        Assert.True(runtime.TryGetSessionActorBinding(
            original.ActorId,
            out var exact));
        Assert.Equal(terminal.Ref.ObjectGeneration, exact.ObjectGeneration);
        Assert.Equal(terminal.Ref.MeshName, exact.MeshName);
    }

    [Fact]
    public async Task Cross_Owner_Rebind_Acknowledges_Tombstone_Before_Source_Swap()
    {
        var events = new List<string> { "new-register" };
        var previous = PreviousBinding("old-node");

        await ZLinkSessionBindingReplacement.CompletePreviousAsync(
            "actor-order",
            RoutingId.From("new-node"),
            previous,
            (request, _) =>
            {
                events.Add("old-tombstone");
                Assert.Equal(previous.BindingToken, request.BindingToken);
                Assert.Equal(previous.BindingGeneration, request.BindingGeneration);
                Assert.Equal(previous.ObjectGeneration, request.ObjectGeneration);
                Assert.Equal(previous.SessionNodeRid, request.SessionNodeRid);
                Assert.Equal(previous.SessionRid, request.SessionRid);
                Assert.Equal(previous.AcceptedHighWater, request.AcceptedHighWater);
                return ValueTask.FromResult(
                    new ZLinkRemoteSessionUnbindResponse(true));
            },
            CancellationToken.None);
        events.Add("source-swap");

        Assert.Equal(
            new[] { "new-register", "old-tombstone", "source-swap" },
            events);
    }

    [Fact]
    public async Task Tombstone_Failure_Or_Cancellation_Leaves_Source_Old_Binding()
    {
        var sourceIsOld = true;
        var previous = PreviousBinding("old-node");

        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkSessionBindingReplacement.CompletePreviousAsync(
                    "actor-failure",
                    RoutingId.From("new-node"),
                    previous,
                    static (_, _) => ValueTask.FromResult(
                        new ZLinkRemoteSessionUnbindResponse(false)),
                    CancellationToken.None)
                .AsTask());
        Assert.True(sourceIsOld);

        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            ZLinkSessionBindingReplacement.CompletePreviousAsync(
                    "actor-cancel",
                    RoutingId.From("new-node"),
                    previous,
                    static (_, token) =>
                        ValueTask.FromCanceled<ZLinkRemoteSessionUnbindResponse>(
                            token),
                    cancellation.Token)
                .AsTask());
        Assert.True(sourceIsOld);
    }

    [Fact]
    public async Task Same_Rebind_Request_Retries_Exact_Tombstone_Until_Acknowledged()
    {
        var previous = PreviousBinding("old-node");
        var requests = new List<ZLinkRemoteSessionUnbindRequest>();

        async ValueTask AttemptAsync()
        {
            await ZLinkSessionBindingReplacement.CompletePreviousAsync(
                "actor-retry",
                RoutingId.From("new-node"),
                previous,
                (request, _) =>
                {
                    requests.Add(request);
                    return ValueTask.FromResult(
                        new ZLinkRemoteSessionUnbindResponse(
                            requests.Count > 1));
                },
                CancellationToken.None);
        }

        await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => AttemptAsync().AsTask());
        await AttemptAsync();

        Assert.Equal(2, requests.Count);
        Assert.Equal(requests[0], requests[1]);
    }

    [Fact]
    public async Task Same_Owner_Replacement_Does_Not_Self_Tombstone()
    {
        var tombstones = 0;
        await ZLinkSessionBindingReplacement.CompletePreviousAsync(
            "actor-same-owner",
            RoutingId.From("same-node"),
            PreviousBinding("same-node"),
            (_, _) =>
            {
                tombstones++;
                return ValueTask.FromResult(
                    new ZLinkRemoteSessionUnbindResponse(true));
            },
            CancellationToken.None);

        Assert.Equal(0, tombstones);
    }

    [Fact]
    public async Task Stale_Exact_Tombstone_Does_Not_Remove_Current_Binding()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-stale-tombstone");
        var actor = new ActorRef(
            "actor-stale-tombstone",
            1,
            "actors",
            RoutingId.From("actor-node"));
        var current = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);

        runtime.UnbindSessionActor(actor.ActorId, context, "stale-token");

        Assert.Same(current, Assert.Single(context.Actors.Bound));
    }

    [Fact]
    public async Task Completed_Route_Commit_Is_Exactly_Fenced_And_Idempotent()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-route-commit");
        var source = new ActorRef(
            "actor-route",
            7,
            "actors",
            RoutingId.From("actor-node-a"));
        var target = new ActorRef(
            "actor-route",
            7,
            "actors",
            RoutingId.From("actor-node-b"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            source,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(source.ActorId, out var identity));
        const string handoffId = "handoff-route-commit";
        Assert.True((await runtime.SealSessionActorRouteAsync(
                new ZLinkSessionRouteSeal(
                source.ActorId,
                identity.BindingToken,
                identity.BindingGeneration,
                source.ObjectGeneration,
                identity.AuthorityOwnerGeneration,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.OwnerLeaseGeneration,
                identity.SessionOwnerNodeGeneration,
                handoffId),
            CancellationToken.None)).Acknowledged);
        var sealedHighWater = identity.AcceptedHighWater;
        Assert.False(runtime.TryAcceptSessionActorFrame(
            source.ActorId,
            identity.BindingToken,
            out var rejectedHighWater));
        Assert.Equal(sealedHighWater, rejectedHighWater);

        var stale = runtime.CommitSessionActorRoute(
            new ZLinkSessionRouteCommit(
                source.ActorId,
                identity.BindingToken,
                identity.BindingGeneration,
                source.ObjectGeneration,
                identity.AuthorityOwnerGeneration + 1,
                identity.AuthorityOwnerGeneration + 2,
                identity.MeshName,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.TargetNodeGeneration + 1,
                identity.OwnerLeaseGeneration,
                identity.OwnerLeaseGeneration + 1,
                identity.SessionOwnerNodeGeneration,
                identity.AcceptedHighWater,
                handoffId,
                target));

        Assert.False(stale.Acknowledged);
        Assert.Equal(source, bound.Ref);

        var command = new ZLinkSessionRouteCommit(
            source.ActorId,
            identity.BindingToken,
            identity.BindingGeneration,
            source.ObjectGeneration,
            identity.AuthorityOwnerGeneration,
            identity.AuthorityOwnerGeneration + 1,
            identity.MeshName,
            identity.MeshName,
            identity.TargetNodeGeneration,
            identity.TargetNodeGeneration + 1,
            identity.OwnerLeaseGeneration,
            identity.OwnerLeaseGeneration + 1,
            identity.SessionOwnerNodeGeneration,
            sealedHighWater,
            handoffId,
            target);
        var committed = runtime.CommitSessionActorRoute(command);
        var retried = runtime.CommitSessionActorRoute(command);

        Assert.True(committed.Acknowledged);
        Assert.True(retried.Acknowledged);
        Assert.Equal(target, bound.Ref);
        Assert.Equal(source.ObjectGeneration, bound.Ref.ObjectGeneration);
        Assert.False(runtime.TryAcceptSessionActorFrame(
            source.ActorId,
            identity.BindingToken,
            out var committedHighWater));
        Assert.Equal(sealedHighWater, committedHighWater);
        var retriedAfterCommit = runtime.CommitSessionActorRoute(command);
        Assert.True(retriedAfterCommit.Acknowledged);
        Assert.Equal(sealedHighWater, retriedAfterCommit.AcceptedHighWater);
        Assert.True(runtime.UnsealCommittedSessionActorRoute(command));
        Assert.True(runtime.UnsealCommittedSessionActorRoute(command));
        Assert.True(runtime.TryAcceptSessionActorFrame(
            source.ActorId,
            identity.BindingToken,
            out var nextHighWater));
        Assert.Equal(committedHighWater + 1, nextHighWater);
        runtime.CompleteAcceptedSessionActorFrame(
            source.ActorId,
            identity.BindingToken);
    }

    [Fact]
    public async Task Sealed_Route_Allows_Target_Push_When_Target_Authority_Generation_Is_Lower()
    {
        var runtime = CreateRuntime();
        var stream = new TestStream(RoutingId.From("session-route-lower"));
        var context = CreateSessionContext(runtime, stream);
        var source = new ActorRef(
            "actor-route-lower",
            7,
            "actors",
            RoutingId.From("actor-node-a"));
        var targetNode = RoutingId.From("actor-node-b");
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            source,
            CancellationToken.None);

        Assert.True(runtime.TryGetSessionActorBinding(source.ActorId, out var identity));
        const string handoffId = "handoff-route-lower";
        Assert.True((await runtime.SealSessionActorRouteAsync(
            new ZLinkSessionRouteSeal(
                source.ActorId,
                identity.BindingToken,
                identity.BindingGeneration,
                source.ObjectGeneration,
                identity.AuthorityOwnerGeneration,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.OwnerLeaseGeneration,
                identity.SessionOwnerNodeGeneration,
                handoffId),
            CancellationToken.None)).Acknowledged);

        var relay = new ZLinkRemoteSessionPushRelay(
            source.ActorId,
            source.ObjectGeneration,
            identity.MeshName,
            targetNode.ToHex(),
            TargetNodeGeneration: 11,
            AuthorityOwnerGeneration: 14,
            OwnerLeaseGeneration: 3,
            identity.BindingToken,
            identity.BindingGeneration,
            identity.SessionOwnerNodeGeneration,
            identity.Context.RoutingId!.Value.ToHex(),
            [1, 2, 3]);

        var wrongTargetNode = RoutingId.From("actor-node-c");
        await runtime.DeliverRemoteSessionPushAsync(
            relay with
            {
                TargetNodeRid = wrongTargetNode.ToHex(),
                TargetNodeGeneration = identity.TargetNodeGeneration,
                AuthorityOwnerGeneration = identity.AuthorityOwnerGeneration,
                OwnerLeaseGeneration = identity.OwnerLeaseGeneration
            },
            [4, 5, 6],
            wrongTargetNode,
            CancellationToken.None);
        Assert.Empty(stream.Writes);

        await runtime.DeliverRemoteSessionPushAsync(
            relay,
            [1, 2, 3],
            targetNode,
            CancellationToken.None);

        Assert.Single(stream.Writes);
        Assert.Equal(new byte[] { 1, 2, 3 }, stream.Writes[0].Payload);

        var target = new ActorRef(
            source.ActorId,
            source.ObjectGeneration,
            source.MeshName,
            targetNode);
        var commit = runtime.CommitSessionActorRoute(
            new ZLinkSessionRouteCommit(
                source.ActorId,
                identity.BindingToken,
                identity.BindingGeneration,
                source.ObjectGeneration,
                identity.AuthorityOwnerGeneration,
                identity.AuthorityOwnerGeneration + 1,
                identity.MeshName,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.TargetNodeGeneration + 1,
                identity.OwnerLeaseGeneration,
                identity.OwnerLeaseGeneration + 1,
                identity.SessionOwnerNodeGeneration,
                identity.AcceptedHighWater,
                handoffId,
                target));
        Assert.True(commit.Acknowledged);

        // The target route is installed before Unseal. The in-flight target
        // push can still carry the source-side fence and must not be dropped.
        var preCommitIdentity = relay with
        {
            AuthorityOwnerGeneration = identity.AuthorityOwnerGeneration,
            TargetNodeGeneration = identity.TargetNodeGeneration,
            OwnerLeaseGeneration = identity.OwnerLeaseGeneration
        };
        await runtime.DeliverRemoteSessionPushAsync(
            preCommitIdentity,
            [7, 8, 9],
            targetNode,
            CancellationToken.None);

        Assert.Equal(2, stream.Writes.Count);
        Assert.Equal(new byte[] { 7, 8, 9 }, stream.Writes[1].Payload);
    }

    [Fact]
    public async Task Ingress_Seal_Waits_For_The_Accepted_Frame_To_Terminate()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-seal-drain");
        var actor = new ActorRef(
            "actor-seal-drain",
            7,
            "actors",
            RoutingId.From("actor-node-a"));
        _ = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var identity));
        Assert.True(runtime.TryAcceptSessionActorFrame(
            actor.ActorId,
            identity.BindingToken,
            out var acceptedHighWater));

        var seal = runtime.SealSessionActorRouteAsync(
                new ZLinkSessionRouteSeal(
                    actor.ActorId,
                    identity.BindingToken,
                    identity.BindingGeneration,
                    actor.ObjectGeneration,
                    identity.AuthorityOwnerGeneration,
                    identity.MeshName,
                    identity.TargetNodeGeneration,
                    identity.OwnerLeaseGeneration,
                    identity.SessionOwnerNodeGeneration,
                    "handoff-drain"),
                CancellationToken.None)
            .AsTask();

        Assert.False(seal.IsCompleted);
        runtime.CompleteAcceptedSessionActorFrame(
            actor.ActorId,
            identity.BindingToken);
        var result = await seal;

        Assert.True(result.Acknowledged);
        Assert.Equal(acceptedHighWater, result.AcceptedHighWater);
        Assert.False(runtime.TryAcceptSessionActorFrame(
            actor.ActorId,
            identity.BindingToken,
            out var postSealHighWater));
        Assert.Equal(acceptedHighWater, postSealHighWater);
    }

    [Fact]
    public async Task Sealed_Route_Holds_Frame_Admission_Until_Unseal()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-seal-unavailable");
        var actor = new ActorRef(
            "actor-seal-unavailable",
            7,
            "actors",
            RoutingId.From("actor-node-a"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(actor.ActorId, out var identity));
        Assert.True((await runtime.SealSessionActorRouteAsync(
                new ZLinkSessionRouteSeal(
                    actor.ActorId,
                    identity.BindingToken,
                    identity.BindingGeneration,
                    actor.ObjectGeneration,
                    identity.AuthorityOwnerGeneration,
                    identity.MeshName,
                    identity.TargetNodeGeneration,
                    identity.OwnerLeaseGeneration,
                    identity.SessionOwnerNodeGeneration,
                "handoff-seal-unavailable"),
            CancellationToken.None)).Acknowledged);

        var wait = runtime.WaitForSessionActorRouteAvailableAsync(
                actor.ActorId,
                identity.BindingToken,
                CancellationToken.None)
            .AsTask();
        Assert.False(wait.IsCompleted);

        var target = new ActorRef(
            actor.ActorId,
            actor.ObjectGeneration,
            actor.MeshName,
            RoutingId.From("actor-node-b"));
        var command = new ZLinkSessionRouteCommit(
            actor.ActorId,
            identity.BindingToken,
            identity.BindingGeneration,
            actor.ObjectGeneration,
            identity.AuthorityOwnerGeneration,
            identity.AuthorityOwnerGeneration + 1,
            identity.MeshName,
            identity.MeshName,
            identity.TargetNodeGeneration,
            identity.TargetNodeGeneration + 1,
            identity.OwnerLeaseGeneration,
            identity.OwnerLeaseGeneration + 1,
            identity.SessionOwnerNodeGeneration,
            identity.AcceptedHighWater,
            "handoff-seal-unavailable",
            target);
        Assert.True(runtime.CommitSessionActorRoute(command).Acknowledged);
        Assert.True(runtime.UnsealCommittedSessionActorRoute(command));
        Assert.True(await wait);

        Assert.True(runtime.TryAcceptSessionActorFrame(
            actor.ActorId,
            identity.BindingToken,
            out var acceptedHighWater));
        Assert.Equal(identity.AcceptedHighWater + 1, acceptedHighWater);
        runtime.CompleteAcceptedSessionActorFrame(
            actor.ActorId,
            identity.BindingToken);
    }

    [Fact]
    public async Task Completed_Route_Commit_Rejects_Each_Stale_Binding_Fence()
    {
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-route-fences");
        var source = new ActorRef(
            "actor-route-fences",
            7,
            "actors",
            RoutingId.From("actor-node-a"));
        var target = new ActorRef(
            source.ActorId,
            source.ObjectGeneration,
            "actors",
            RoutingId.From("actor-node-b"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            source,
            CancellationToken.None);
        Assert.True(runtime.TryGetSessionActorBinding(source.ActorId, out var identity));
        const string handoffId = "handoff-route-fences";
        Assert.True((await runtime.SealSessionActorRouteAsync(
            new ZLinkSessionRouteSeal(
                source.ActorId,
                identity.BindingToken,
                identity.BindingGeneration,
                source.ObjectGeneration,
                identity.AuthorityOwnerGeneration,
                identity.MeshName,
                identity.TargetNodeGeneration,
                identity.OwnerLeaseGeneration,
                identity.SessionOwnerNodeGeneration,
                handoffId),
            CancellationToken.None)).Acknowledged);

        var current = new ZLinkSessionRouteCommit(
            source.ActorId,
            identity.BindingToken,
            identity.BindingGeneration,
            source.ObjectGeneration,
            identity.AuthorityOwnerGeneration,
            identity.AuthorityOwnerGeneration + 1,
            identity.MeshName,
            identity.MeshName,
            identity.TargetNodeGeneration,
            identity.TargetNodeGeneration + 1,
            identity.OwnerLeaseGeneration,
            identity.OwnerLeaseGeneration + 1,
            identity.SessionOwnerNodeGeneration,
            identity.AcceptedHighWater,
            handoffId,
            target);
        var staleCommands = new[]
        {
            current with { BindingToken = "stale-binding" },
            current with { BindingGeneration = current.BindingGeneration + 1 },
            current with { ObjectGeneration = current.ObjectGeneration + 1 },
            current with
            {
                PreviousAuthorityOwnerGeneration =
                    current.PreviousAuthorityOwnerGeneration + 1
            },
            current with { PreviousMeshName = "stale-mesh" },
            current with
            {
                PreviousTargetNodeGeneration =
                    current.PreviousTargetNodeGeneration + 1
            },
            current with
            {
                PreviousOwnerLeaseGeneration =
                    current.PreviousOwnerLeaseGeneration + 1
            },
            current with
            {
                SessionOwnerNodeGeneration =
                    current.SessionOwnerNodeGeneration + 1
            },
            current with { AcceptedHighWater = current.AcceptedHighWater + 1 },
            current with
            {
                TargetActor = new ActorRef(
                    target.ActorId,
                    target.ObjectGeneration + 1,
                    target.MeshName,
                    target.NodeRid)
            }
        };

        foreach (var stale in staleCommands)
        {
            Assert.False(runtime.CommitSessionActorRoute(stale).Acknowledged);
            Assert.Equal(source, bound.Ref);
        }
    }

    [Fact]
    public async Task Bound_Actor_Relay_Does_Not_Resolve_The_Location_Store_Per_Message()
    {
        var directory = new MissingActorDirectory();
        var runtime = CreateRuntime(actorDirectory: directory);
        var context = CreateSessionContext(runtime, "session-rid");
        var actor = new ActorRef("actor-1", 1, "actors", RoutingId.From("actor-node"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            actor,
            CancellationToken.None);
        using var payload = Message.From(new byte[] { 1, 2, 3 });
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(1),
            "ActorPingReq",
            ZlinkStreamMetadata.Empty);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await context.ActorCoordinator.RelayToActorAsync(
                bound,
                header,
                payload,
                static (_, _, _) => ValueTask.CompletedTask,
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(0, directory.Calls);
        Assert.Equal(actor, Assert.Single(context.Actors.Bound).Ref);
    }

    [Fact]
    public async Task Bound_Actor_Relay_Keeps_The_Exact_Bound_Route_Until_Relocation_Switches_It()
    {
        var current = new ActorRef("actor-1", 2, "actors", RoutingId.From("actor-node-b"));
        var directory = new FixedActorDirectory(current);
        var runtime = CreateRuntime(actorDirectory: directory);
        var context = CreateSessionContext(runtime, "session-rid");
        var stale = new ActorRef("actor-1", 1, "actors", RoutingId.From("actor-node-a"));
        var bound = await context.ActorCoordinator.BindOrGetActorAsync(
            context,
            stale,
            CancellationToken.None);
        using var payload = Message.From(new byte[] { 1, 2, 3 });
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "ActorPingReq",
            ZlinkStreamMetadata.Empty);

        await Assert.ThrowsAnyAsync<Exception>(async () =>
            await context.ActorCoordinator.RelayToActorAsync(
                bound,
                header,
                payload,
                static (_, _, _) => ValueTask.CompletedTask,
                CancellationToken.None));

        var retained = Assert.Single(context.Actors.Bound);
        Assert.Same(bound, retained);
        Assert.Equal(stale, retained.Ref);
        Assert.Equal(0, directory.Calls);
    }

    [Fact]
    public void Session_Owner_Tombstone_Rejects_Every_Late_Commit_Until_Expiry()
    {
        var time = new ManualTimeProvider();
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromSeconds(1),
            time,
            maxTombstones: 2);
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-tombstone");
        var sessionRid = context.RoutingId!.Value;
        var route = SessionBindingRoute(
            "actor-tombstone",
            targetNodeGeneration: 3);
        var actor = new ZLinkSessionActor(
            context,
            route.Ref.ActorId,
            sessionRid,
            "binding-tombstone");
        _ = table.Bind(
            route.Ref.ActorId,
            context,
            actor.BindingToken,
            actor,
            bindingGeneration: 5,
            route,
            sessionOwnerNodeGeneration: 7);

        table.Tombstone(
            route.Ref.ActorId,
            sessionRid,
            actor.BindingToken,
            bindingGeneration: 5,
            sessionOwnerNodeGeneration: 7,
            route);

        Assert.Equal(1, table.TombstoneCount);
        AssertLateCommitRejected();
        AssertLateCommitRejected();
        Assert.Equal(1, table.TombstoneCount);

        time.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(0, table.TombstoneCount);
        _ = table.Bind(
            route.Ref.ActorId,
            context,
            actor.BindingToken,
            actor,
            bindingGeneration: 5,
            route,
            sessionOwnerNodeGeneration: 7);
        Assert.True(table.TryGet(
            route.Ref.ActorId,
            actor.BindingToken,
            out ZLinkSessionBindingEntry _));
        return;

        void AssertLateCommitRejected()
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                table.Bind(
                    route.Ref.ActorId,
                    context,
                    actor.BindingToken,
                    actor,
                    bindingGeneration: 5,
                    route,
                    sessionOwnerNodeGeneration: 7));
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
            Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
        }
    }

    [Fact]
    public void Session_Owner_Tombstone_Requires_The_Full_Actor_Route_Fence()
    {
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromSeconds(30));
        var runtime = CreateRuntime();
        var context = CreateSessionContext(runtime, "session-full-fence");
        var sessionRid = context.RoutingId!.Value;
        var route = SessionBindingRoute(
            "actor-full-fence",
            targetNodeGeneration: 3);
        var actor = new ZLinkSessionActor(
            context,
            route.Ref.ActorId,
            sessionRid,
            "binding-full-fence");
        _ = table.Bind(
            route.Ref.ActorId,
            context,
            actor.BindingToken,
            actor,
            bindingGeneration: 5,
            route,
            sessionOwnerNodeGeneration: 7);
        var reusedRidFromAnotherLifecycle = SessionBindingRoute(
            route.Ref.ActorId,
            targetNodeGeneration: 4);

        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            table.Tombstone(
                route.Ref.ActorId,
                sessionRid,
                actor.BindingToken,
                bindingGeneration: 5,
                sessionOwnerNodeGeneration: 7,
                reusedRidFromAnotherLifecycle));

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.True(table.TryGet(
            route.Ref.ActorId,
            actor.BindingToken,
            out ZLinkSessionBindingEntry retained));
        Assert.Equal(route, retained.Route);
        Assert.Equal(0, table.TombstoneCount);
    }

    [Fact]
    public void Session_Owner_Tombstone_Capacity_Fails_Closed()
    {
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromMinutes(1),
            maxTombstones: 2);
        var routeA = SessionBindingRoute("actor-capacity-a", 1);
        var routeB = SessionBindingRoute("actor-capacity-b", 1);
        var routeC = SessionBindingRoute("actor-capacity-c", 1);

        AddTombstone(routeA, "session-a", "binding-a");
        AddTombstone(routeB, "session-b", "binding-b");
        var capacity = Assert.Throws<ZLinkFrameworkException>(() =>
            AddTombstone(routeC, "session-c", "binding-c"));

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, capacity.Kind);
        Assert.Equal(2, table.TombstoneCount);
        return;

        void AddTombstone(
            ZLinkSessionBindingRoute route,
            string session,
            string token)
        {
            table.Tombstone(
                route.Ref.ActorId,
                RoutingId.From(session),
                token,
                bindingGeneration: 1,
                sessionOwnerNodeGeneration: 1,
                route);
        }
    }

    private static ZLinkSessionBindingRoute SessionBindingRoute(
        string actorId,
        ulong targetNodeGeneration)
    {
        const string meshName = "actors";
        return ZLinkSessionBindingRoute.Create(
            new ActorRef(
                actorId,
                11,
                meshName,
                RoutingId.From("actor-node")),
            meshName,
            targetNodeGeneration,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 17);
    }

    private static ZLinkSessionContext CreateSessionContext(
        ZLinkFrameworkRuntime runtime,
        string sessionRid)
    {
        return CreateSessionContext(runtime, new TestStream(RoutingId.From(sessionRid)));
    }

    private static ZLinkSessionContext CreateSessionContext(
        ZLinkFrameworkRuntime runtime,
        TestStream stream)
    {
        return new ZLinkSessionContext(
            runtime,
            stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
    }

    private static ZLinkRemoteSessionPreviousBinding PreviousBinding(
        string nodeRid) => new(
        RoutingId.From(nodeRid).ToBytes().ToArray(),
        RoutingId.From("session-node").ToBytes().ToArray(),
        RoutingId.From("session-rid").ToBytes().ToArray(),
        "old-token",
        7,
        11,
        "actors",
        13,
        17,
        19,
        23,
        29);

    private static ZLinkFrameworkRuntime CreateRuntime(
        IZLinkActorResolver? actorDirectory = null,
        TimeSpan? defaultRequestTimeout = null)
    {
        var registration = new ZLinkFrameworkRegistration();
        if (defaultRequestTimeout is { } timeout)
            registration.DefaultRequestTimeout = timeout;
        var services = new ServiceCollection();
        services.AddSingleton(registration);
        if (actorDirectory is not null) services.AddSingleton(actorDirectory);
        var provider = services.BuildServiceProvider();

        return new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
    }

    private sealed record SessionPush(string Value);

    private sealed class MissingActorDirectory : IZLinkActorResolver
    {
        public int Calls { get; private set; }

        public ValueTask<(ActorRef? Ref, bool RowPresent)> FindWithPresenceAsync(
            string actorId,
            CancellationToken cancellationToken = default)
        {
            Calls++;
            return ValueTask.FromResult<(ActorRef?, bool)>((null, false));
        }
    }

    private sealed class FixedActorDirectory(ActorRef actor) : IZLinkActorResolver
    {
        public int Calls { get; private set; }

        public ValueTask<(ActorRef? Ref, bool RowPresent)> FindWithPresenceAsync(
            string actorId,
            CancellationToken cancellationToken = default)
        {
            Calls++;
            return ValueTask.FromResult<(ActorRef?, bool)>(
                actor.ActorId == actorId ? (actor, true) : (null, false));
        }
    }

    private sealed class TestSessionHandlerRegistry : IZLinkSessionHandlerRegistry
    {
        public void AddHandler<THandler>() where THandler : class { }

        public void AddHandler<THandler>(string packetName) where THandler : class { }

        public ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default) => ValueTask.FromResult(false);
    }

    private sealed class TestStream(RoutingId routingId, bool acceptsWrites = true) : IZLinkStream
    {
        public string SessionId { get; } = routingId.ToHex();

        public RoutingId? RoutingId { get; } = routingId;

        public string? LocalAddr => null;

        public string? RemoteAddr => null;

        public SendFlags LastWriteFlags { get; private set; }

        public List<(byte[] Payload, SendFlags Flags)> Writes { get; } = [];

        public bool Write(
            ZLinkMessage payload,
            SendFlags flags = SendFlags.None)
        {
            LastWriteFlags = flags;
            Writes.Add((payload.Decode<byte[]>(), flags));
            return acceptsWrites;
        }

        public ValueTask CloseAsync()
        {
            return ValueTask.CompletedTask;
        }
    }
}
