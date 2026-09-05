using Systems.Zlink.Framework.Runtime.Protocol;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class CanonicalActorJoinIngressReplyTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ManagedSource_Command28Request_ConsumesCommand20TailAndApplicationReply(
        bool targetDialsSource)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "mesh");
        await using var target = new ZLinkManagedMeshNode(context, "mesh");
        await using var targetMonitor = target.OpenMonitor();
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"actor-join-source-{suffix}");
        var targetRid = RoutingId.From($"actor-join-target-{suffix}");
        var sourceEndpoint = AllocateTcpEndpoint();
        var targetEndpoint = AllocateTcpEndpoint();
        const string targetSpotId = "target-spot";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        if (targetDialsSource)
            target.ConnectPeer(sourceEndpoint, sourceRid);
        else
            source.ConnectPeer(targetEndpoint, targetRid);
        var targetSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            targetSpotId,
            out var created);
        Assert.True(created);
        source.ObserveSpotAuthority(
            targetRid,
            targetSpotId,
            targetSpot.LifecycleGeneration,
            target.Status().LifecycleGeneration,
            targetSpot.AuthorityOwnerGeneration,
            ownerLeaseGeneration: 7);
        target.Start();
        source.Start();

        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        var operationId = source.AllocateOperationId();
        var request = new ZLinkBackendCanonicalActorJoinRequest(
            new ZLinkBackendActorRef(sourceRid, "actor-1", 11),
            source.Status().LifecycleGeneration,
            ActorAuthorityOwnerGeneration: 3,
            ActorOwnerLeaseGeneration: 5,
            Entry: false,
            targetRid,
            targetSpotId,
            targetSpot.LifecycleGeneration,
            target.Status().LifecycleGeneration,
            targetSpot.AuthorityOwnerGeneration,
            TargetOwnerLeaseGeneration: 7,
            "ZLinkFrameworkActorJoinRequest",
            "application/json",
            "{\"request\":true}"u8.ToArray());

        Assert.Equal(
            SubmitResult.Ok,
            source.TryRequestCanonicalActorJoin(
                request,
                operationId,
                TimeSpan.FromSeconds(2)));

        // The target queues command 28 only when Core delivered a Request with
        // a request sequence. A one-way command 28 is rejected before this
        // record can be observed.
        var ingress = await ReceiveActorJoinAsync(target);
        using var applicationReply = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.Encode(
                "ActorJoinReply",
                "application/json",
                "{\"accepted\":true}"u8));
        Assert.Equal(
            SubmitResult.Ok,
            ingress.ReplyJoin(
                ActorJoinResult.Accepted,
                [applicationReply]));

        var (completion, replyParts) = await ReceiveCompletionAsync(
            source,
            operationId);
        try
        {
            Assert.Equal(MeshRecordKind.Completion, completion.Kind);
            Assert.Equal(MeshOperationKind.ActorJoin, completion.OperationKind);
            Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
            Assert.Equal(0, completion.FailureErrno);
            var join = Assert.IsType<ActorJoinCompletion>(
                completion.JoinCompletion);
            Assert.Equal(ActorJoinResult.Accepted, join.JoinResult);
            Assert.Equal(targetRid, join.Actor.NodeRid);
            Assert.Equal(targetSpotId, join.Location.SpotId);
            Assert.Equal(
                targetSpot.LifecycleGeneration,
                join.Location.SpotGeneration);
            Assert.Equal(
                checked((uint)ZLinkRemoteActorJoinPackets
                    .ConservativeReceiveChunkLimitBytes),
                join.ReceiveChunkLimitBytes);
            Assert.Equal("application/json", join.ReplyContentType);

            var applicationFrame = Assert.Single(replyParts);
            Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecode(
                applicationFrame.AsReadOnlyMemory(),
                out var application));
            Assert.Equal("ActorJoinReply", application.PacketName);
            Assert.Equal("application/json", application.ContentType);
            Assert.Equal(
                "{\"accepted\":true}"u8.ToArray(),
                application.Payload.ToArray());
            Assert.Equal(0UL, targetMonitor.Status().ProtocolErrors);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
    }

    [Fact]
    public async Task ManagedSource_Command28Request_Retries_Same_Correlation_After_Lost_Reply()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        ReplySubmitOperation? firstReplyRoute = null;
        var replySubmits = 0;
        await using var source = new ZLinkManagedMeshNode(context, "mesh");
        await using var target = new ZLinkManagedMeshNode(
            context,
            "mesh",
            nativeTerminalReplySubmitOverride: reply =>
            {
                firstReplyRoute ??= reply;
                if (ReferenceEquals(firstReplyRoute, reply))
                    return SubmitResult.Ok;
                reply.Submit();
                Interlocked.Increment(ref replySubmits);
                return SubmitResult.Ok;
            });
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"actor-join-source-{suffix}");
        var targetRid = RoutingId.From($"actor-join-target-{suffix}");
        var sourceEndpoint = AllocateTcpEndpoint();
        var targetEndpoint = AllocateTcpEndpoint();
        const string targetSpotId = "target-spot";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.ConnectPeer(sourceEndpoint, sourceRid);
        var targetSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            targetSpotId,
            out var created);
        Assert.True(created);
        source.ObserveSpotAuthority(
            targetRid,
            targetSpotId,
            targetSpot.LifecycleGeneration,
            target.Status().LifecycleGeneration,
            targetSpot.AuthorityOwnerGeneration,
            ownerLeaseGeneration: 7);
        source.Start();
        target.Start();

        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        var operationId = source.AllocateOperationId();
        var request = new ZLinkBackendCanonicalActorJoinRequest(
            new ZLinkBackendActorRef(sourceRid, "actor-lost-reply", 11),
            source.Status().LifecycleGeneration,
            ActorAuthorityOwnerGeneration: 3,
            ActorOwnerLeaseGeneration: 5,
            Entry: false,
            targetRid,
            targetSpotId,
            targetSpot.LifecycleGeneration,
            target.Status().LifecycleGeneration,
            targetSpot.AuthorityOwnerGeneration,
            TargetOwnerLeaseGeneration: 7,
            "ZLinkFrameworkActorJoinRequest",
            "application/json",
            "{}"u8.ToArray());

        var admissionDeadline = DateTimeOffset.UtcNow.AddSeconds(8)
            .ToUnixTimeMilliseconds();
        Assert.Equal(
            SubmitResult.Ok,
            source.TryRequestCanonicalActorJoin(
                request,
                operationId,
                TimeSpan.FromSeconds(8)));

        var firstIngress = await ReceiveActorJoinAsync(target);
        Assert.Equal(new MeshOperationId(0, operationId.Low), firstIngress.OperationId);
        var admissions = new ZLinkActorHandoffAdmissions();
        var executions = 0;
        ValueTask<ZLinkRemoteActorAdmissionReply> AdmitAsync(MeshReceiveRecord ingress) =>
            admissions.AdmitAsync(
                new ZLinkRemoteActorAdmissionRequest(
                    request.Actor.ActorId,
                    "Sample.Actor",
                    "source-spot",
                    sourceRid.ToBytes().ToArray(),
                    request.ContentType,
                    request.ApplicationPayload.ToArray(),
                    ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                        sourceRid,
                        request.Actor.ActorId,
                        request.Actor.Generation,
                        request.ActorNodeGeneration,
                        ingress.OperationId.Low),
                    admissionDeadline),
                targetSpotId,
                _ =>
                {
                    Interlocked.Increment(ref executions);
                    return ValueTask.FromResult(new ZLinkRemoteActorAdmissionReply(
                        false,
                        "application/json",
                        "{\"reason\":\"ZoneMaintenance\"}"u8.ToArray(),
                        admissionDeadline));
                },
                CancellationToken.None);
        var firstDecision = await AdmitAsync(firstIngress);
        using var firstReply = Message.From(firstDecision.Reply);
        Assert.Equal(
            SubmitResult.Ok,
            firstIngress.ReplyJoin(
                ActorJoinResult.Rejected,
                [firstReply]));

        // The lower source RID's outbound direction wins reciprocal HANDOVER.
        // Core ends the admitted attempt with NOT_CONNECTED, so the sender can
        // replay inside the original deadline without an artificial attempt cut.
        source.ConnectPeer(targetEndpoint, targetRid);

        var retryIngress = await ReceiveActorJoinAsync(
            target,
            TimeSpan.FromSeconds(7));
        Assert.Equal(firstIngress.OperationId, retryIngress.OperationId);
        var retryDecision = await AdmitAsync(retryIngress);
        Assert.Same(firstDecision, retryDecision);
        Assert.Equal(1, Volatile.Read(ref executions));
        using var retryReply = Message.From(retryDecision.Reply);
        Assert.Equal(
            SubmitResult.Ok,
            retryIngress.ReplyJoin(
                ActorJoinResult.Rejected,
                [retryReply]));

        var (completion, replyParts) = await ReceiveCompletionAsync(
            source,
            operationId);
        try
        {
            Assert.Equal(operationId, completion.OperationId);
            Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
            var join = Assert.IsType<ActorJoinCompletion>(
                completion.JoinCompletion);
            Assert.Equal(ActorJoinResult.Rejected, join.JoinResult);
            var applicationReply = Assert.Single(replyParts);
            Assert.Equal(
                "{\"reason\":\"ZoneMaintenance\"}"u8.ToArray(),
                applicationReply.AsReadOnlyMemory().ToArray());
            Assert.Equal(1, Volatile.Read(ref replySubmits));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
            await admissions.ResetGenerationAsync();
        }
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_TerminatesOnNativeReplyChannelExactlyOnce()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var target = new ZLinkManagedMeshNode(context, "mesh");
        await using var monitor = target.OpenMonitor();
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"actor-join-source-{suffix}");
        var targetRid = RoutingId.From($"actor-join-target-{suffix}");
        var targetEndpoint = $"inproc://actor-join-target-{suffix}";
        var sourceEndpoint = $"inproc://actor-join-source-{suffix}";
        const string targetSpotId = "target-spot";

        target.SetRoutingId(targetRid);
        target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        target.SetBind(targetEndpoint);
        var targetSpot = target.GetOrCreateSpot(targetSpotId, out var created);
        Assert.True(created);
        target.Start();

        await using var source = context.CreateDealerSocket();
        source.SetRoutingId(sourceRid);
        source.Connect(targetEndpoint);
        using (var hello = Message.From(
                   ZLinkServiceWireCodec.EncodeRouteAdmission(
                       ServiceWireConstants.Command.Hello,
                       "mesh",
                       sourceEndpoint,
                       lifecycleGeneration: 1,
                       descriptorRevision: 1,
                       new Dictionary<string, uint>(StringComparer.Ordinal),
                       objectRole: (byte)ZLinkMeshNodeObjectRole.Server)))
            await source.Send().Message(hello).Async(CancellationToken.None);

        await WaitUntilAsync(() => target.Status().AdmittedPeerCount == 1);
        using var admission = await ReceiveAsync(source);

        var acceptedRequest = CreateRequest(
            correlation: 41,
            sourceRid,
            targetRid,
            target.Status().LifecycleGeneration,
            targetSpotId,
            targetSpot.LifecycleGeneration);
        var acceptedReplyTask = SendRequestAsync(source, acceptedRequest);
        var acceptedIngress = await ReceiveActorJoinAsync(target);
        Assert.Equal(
            SubmitResult.Ok,
            acceptedIngress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));
        Assert.Equal(
            SubmitResult.InvalidState,
            acceptedIngress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));

        var acceptedReply = await acceptedReplyTask.WaitAsync(
            TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(acceptedReply, acceptedRequest.Correlation);
            Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
                reply,
                out var completion,
                out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(ActorJoinResult.Accepted, completion!.JoinResult);
            Assert.Equal(targetSpotId, completion.Spot!.Value.SpotId);
            Assert.Equal(targetSpot.LifecycleGeneration,
                completion.Spot.Value.SpotGeneration);
            Assert.Equal(
                checked((uint)ZLinkRemoteActorJoinPackets
                    .ConservativeReceiveChunkLimitBytes),
                completion.ReceiveChunkLimitBytes);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(acceptedReply);
        }

        var errorRequest = acceptedRequest with { Correlation = 42 };
        var errorReplyTask = SendRequestAsync(source, errorRequest);
        var errorIngress = await ReceiveActorJoinAsync(target);
        Assert.Equal(
            SubmitResult.Ok,
            errorIngress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));
        Assert.Equal(
            SubmitResult.InvalidState,
            errorIngress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));

        var errorReply = await errorReplyTask.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(errorReply, errorRequest.Correlation);
            Assert.Equal((int)RequestResult.InternalError, reply.TerminalResult);
            Assert.Equal(
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                reply.FailureCode);
            Assert.Empty(reply.Tail);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(errorReply);
        }

        var rejectedRequest = acceptedRequest with
        {
            Correlation = 43,
            TargetNodeGeneration = checked(
                target.Status().LifecycleGeneration + 1)
        };
        var rejectedReply = await SendRequestAsync(source, rejectedRequest)
            .WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(rejectedReply, rejectedRequest.Correlation);
            Assert.Equal((int)RequestResult.ProtocolError, reply.TerminalResult);
            Assert.Equal(
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError,
                reply.FailureCode);
            Assert.Empty(reply.Tail);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(rejectedReply);
        }

        var protocolErrors = monitor.Status().ProtocolErrors;
        var oneWayRequest = acceptedRequest with { Correlation = 44 };
        using (var head = Message.From(
                   ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(oneWayRequest)))
        using (var payload = Message.From(
                   ZLinkApplicationPayloadEnvelopeCodec.Encode(
                       "ZLinkFrameworkActorJoinRequest",
                       "application/json",
                       "{}"u8)))
            await source.Send()
                .Message(head)
                .Message(payload)
                .Async(CancellationToken.None);
        await WaitUntilAsync(() =>
            monitor.Status().ProtocolErrors > protocolErrors);
        Assert.Equal(0UL, target.Status().PendingApplicationMessages);
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_RetriesPreservedNativeReplyAfterBackpressure()
    {
        var attempts = 0;
        var wireSubmits = 0;
        await using var runtime = await ConnectedRuntime.CreateAsync(reply =>
        {
            if (Interlocked.Increment(ref attempts) == 1)
                return SubmitResult.Backpressured;
            reply.Submit();
            Interlocked.Increment(ref wireSubmits);
            return SubmitResult.Ok;
        });
        var request = CreateRequest(
            correlation: 51,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);

        var replyTask = SendRequestAsync(runtime.Source, request);
        var ingress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Backpressured,
            ingress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));

        var parts = await replyTask.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            Assert.Equal(2, Volatile.Read(ref attempts));
            Assert.Equal(1, Volatile.Read(ref wireSubmits));
            var reply = DecodeReply(parts, request.Correlation);
            Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
                reply,
                out var completion,
                out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(ActorJoinResult.Accepted, completion!.JoinResult);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_FinalRetryFailureConsumesReplyGate()
    {
        var attempts = 0;
        await using var runtime = await ConnectedRuntime.CreateAsync(_ =>
        {
            if (Interlocked.Increment(ref attempts) == 1)
                return SubmitResult.Backpressured;
            return SubmitResult.Terminated;
        });
        await using var monitor = runtime.Target.OpenMonitor();
        var request = CreateRequest(
            correlation: 53,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);

        _ = SendRequestAsync(runtime.Source, request);
        var ingress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Backpressured,
            ingress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));

        await WaitUntilAsync(() =>
            Volatile.Read(ref attempts) == 2
            && monitor.Status().ProtocolErrors != 0);
        Assert.Equal(
            SubmitResult.InvalidState,
            ingress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));
        Assert.Equal(2, Volatile.Read(ref attempts));
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_OpaqueReplyStopsOnlyOnCoreTerminal()
    {
        var attempts = 0;
        var coreTerminal = 0;
        await using var runtime = await ConnectedRuntime.CreateAsync(_ =>
        {
            Interlocked.Increment(ref attempts);
            return Volatile.Read(ref coreTerminal) == 0
                ? SubmitResult.Backpressured
                : SubmitResult.Terminated;
        });
        await using var monitor = runtime.Target.OpenMonitor();
        var staleRequest = CreateRequest(
            correlation: 54,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);
        _ = SendRequestAsync(runtime.Source, staleRequest);
        var staleIngress = await ReceiveActorJoinAsync(runtime.Target);
        var pendingRequest = staleRequest with { Correlation = 55 };

        _ = SendRequestAsync(runtime.Source, pendingRequest);
        var pendingIngress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Backpressured,
            pendingIngress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));

        await runtime.DisconnectSourceAsync();
        var attemptsBeforeCoreTerminal = Volatile.Read(ref attempts);
        await WaitUntilAsync(() =>
            Volatile.Read(ref attempts) > attemptsBeforeCoreTerminal);
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        Volatile.Write(ref coreTerminal, 1);
        await WaitUntilAsync(() => monitor.Status().ProtocolErrors != 0);
        var attemptsAtDiscard = Volatile.Read(ref attempts);
        await runtime.ReconnectAsync();
        Assert.Equal(
            SubmitResult.Terminated,
            staleIngress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));
        Assert.Equal(attemptsAtDiscard + 1, Volatile.Read(ref attempts));
        await Task.Delay(150);
        Assert.Equal(attemptsAtDiscard + 1, Volatile.Read(ref attempts));
        Assert.Equal(
            SubmitResult.InvalidState,
            pendingIngress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_HandoverLeavesReplyRouteToCore()
    {
        var attempts = 0;
        var submitMode = 0;
        await using var runtime = await ConnectedRuntime.CreateAsync(reply =>
        {
            Interlocked.Increment(ref attempts);
            if (Volatile.Read(ref submitMode) == 0)
                return SubmitResult.Backpressured;
            if (Volatile.Read(ref submitMode) == 1)
                return SubmitResult.Terminated;
            reply.Submit();
            return SubmitResult.Ok;
        });
        await using var monitor = runtime.Target.OpenMonitor();
        var staleRequest = CreateRequest(
            correlation: 56,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);

        _ = SendRequestAsync(runtime.Source, staleRequest);
        var staleIngress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Backpressured,
            staleIngress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));

        var protocolErrors = monitor.Status().ProtocolErrors;
        await runtime.HandoverAsync();
        await WaitUntilAsync(() =>
            Volatile.Read(ref attempts) > 1);
        Assert.Equal(protocolErrors, monitor.Status().ProtocolErrors);
        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);

        await runtime.SendPriorHelloAsync();
        // The old Hello's Admit follows the current logical RID route. Consume
        // that DATA before waiting for a REPLY, which cannot overtake it.
        using var priorAdmission = await ReceiveAsync(runtime.Source);
        await Task.Delay(150);
        Assert.Equal(protocolErrors, monitor.Status().ProtocolErrors);
        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);

        await runtime.DisconnectPriorSourceAsync();
        await Task.Delay(150);
        Assert.Equal(protocolErrors, monitor.Status().ProtocolErrors);
        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);
        Volatile.Write(ref submitMode, 1);
        await WaitUntilAsync(() =>
            monitor.Status().ProtocolErrors > protocolErrors);
        var attemptsAfterDisconnect = Volatile.Read(ref attempts);
        await Task.Delay(150);
        Assert.Equal(attemptsAfterDisconnect, Volatile.Read(ref attempts));
        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);

        Volatile.Write(ref submitMode, 2);
        var replacementRequest = staleRequest with { Correlation = 57 };
        var replacementReplyTask = SendRequestAsync(runtime.Source, replacementRequest);
        var replacementIngress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Ok,
            replacementIngress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));

        var replacementReply = await replacementReplyTask.WaitAsync(
            TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(replacementReply, replacementRequest.Correlation);
            Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
                reply,
                out var completion,
                out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(ActorJoinResult.Accepted, completion!.JoinResult);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replacementReply);
        }
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_IdempotentHelloThenDisconnect_RemovesAdmittedPeerPromptly()
    {
        await using var runtime = await ConnectedRuntime.CreateAsync(_ =>
            SubmitResult.Ok);

        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);
        await runtime.SendIdempotentHelloAsync();
        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);

        await runtime.DisconnectSourceAsync();
        await WaitUntilAsync(() =>
            runtime.Target.Status().AdmittedPeerCount == 0);
    }

    [Fact]
    public async Task RouteAdmission_PriorHelloThenExactDisconnect_DoesNotReplaceCurrentPeer()
    {
        await using var runtime = await ConnectedRuntime.CreateAsync(_ =>
            SubmitResult.Ok);

        for (var iteration = 0; iteration < 6; iteration++)
        {
            await runtime.HandoverAsync();
            await runtime.SendPriorHelloAndDisconnectAsync();

            // The old Hello may either be rejected or be discarded with its
            // closing transport. In both cases it must never steal the logical
            // RID: the current pair still has to receive its exact Admit reply.
            await Task.Delay(50);
            await runtime.SendIdempotentHelloAsync();
            Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);
        }
    }

    [Fact]
    public async Task HandoverAdmissionFailure_DisposeClosesUnpublishedReplacement()
    {
        var runtime = await ConnectedRuntime.CreateAsync(_ => SubmitResult.Ok);
        try
        {
            await Assert.ThrowsAsync<TimeoutException>(() =>
                runtime.HandoverAsync(
                    admitReplacement: static (_, _) =>
                        Task.FromException<Received>(new TimeoutException(
                            "Route admission reply was not received."))));
        }
        finally
        {
            await runtime.DisposeAsync().AsTask().WaitAsync(
                TimeSpan.FromSeconds(10));
        }
    }

    [Fact]
    public async Task RouteAdmission_HandoverStartsFreshLivenessDeadline()
    {
        await using var runtime = await ConnectedRuntime.CreateAsync(_ =>
            SubmitResult.Ok);

        await Task.Delay(ZLinkServiceLiveness.PeerTimeout - TimeSpan.FromSeconds(2));
        await runtime.HandoverAsync();
        await Task.Delay(TimeSpan.FromSeconds(3));

        Assert.Equal(1UL, runtime.Target.Status().AdmittedPeerCount);
    }

    [Fact]
    public async Task RouterMonitor_HandoverReportsCurrentConnectionIdentity()
    {
        Assert.Null(typeof(MonitorEvent).GetProperty("TransportPairId"));
        Assert.Null(typeof(MonitorEvent).GetProperty("TransportPairGeneration"));
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var router = context.CreateRouterSocket();
        router.Options.Handover = true;
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"monitor-source-{suffix}");
        router.SetRoutingId(RoutingId.From($"monitor-target-{suffix}"));
        var endpoint = AllocateTcpEndpoint();
        router.Bind(endpoint);
        using var monitor = router.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);

        await using var first = context.CreateDealerSocket();
        first.SetRoutingId(sourceRid);
        first.Connect(endpoint);
        var firstReady = await ReceiveMonitorEventAsync(
            monitor,
            MonitorEventType.ConnectionReady);

        await using var replacement = context.CreateDealerSocket();
        replacement.SetRoutingId(sourceRid);
        replacement.Connect(endpoint);
        var replacementReady = await ReceiveMonitorEventAsync(
            monitor,
            MonitorEventType.ConnectionReady);

        Assert.Equal(sourceRid, firstReady.RoutingId);
        Assert.Equal(sourceRid, replacementReady.RoutingId);
        Assert.NotEqual(0UL, firstReady.ConnectionId);
        Assert.NotEqual(
            (firstReady.ConnectionId, firstReady.TransportLane),
            (replacementReady.ConnectionId, replacementReady.TransportLane));

        await first.DisposeAsync();
        var disconnected = await ReceiveMonitorEventAsync(
            monitor,
            MonitorEventType.Disconnected);
        Assert.Equal(firstReady.ConnectionId, disconnected.ConnectionId);
        Assert.Equal(firstReady.TransportLane, disconnected.TransportLane);
    }

    [Fact]
    public async Task ActorCreateCompletion_AfterHandoverTimeout_ReplaysOnCurrentRoute()
    {
        await using var runtime = await ConnectedRuntime.CreateAsync(reply =>
        {
            reply.Submit();
            return SubmitResult.Ok;
        });
        var operationTarget = new DeferredActorCreateOperationTarget();
        runtime.Target.SetActorCreateOperationTarget(operationTarget);
        const ulong correlation = 58;
        var targetGeneration = runtime.Target.Status().LifecycleGeneration;
        var reservation = new ObjectReservationFence(
            "actor-reservation",
            "actor-store-version",
            71,
            73,
            runtime.TargetRid,
            targetGeneration,
            "target-owner",
            79,
            1);
        var operation = new ActorCreateOperation(
            correlation,
            new MeshOperationId(83, 89),
            runtime.SourceRid,
            1,
            "remote-actor",
            "Sample.RemoteActor",
            reservation,
            checked((ulong)DateTimeOffset.UtcNow
                .AddSeconds(5)
                .ToUnixTimeMilliseconds()));

        var replyTask = SendActorCreateRequestAsync(runtime.Source, operation);
        await operationTarget.Started.WaitAsync(TimeSpan.FromSeconds(2));

        // A duplicate/replacement Hello can race with a slow Store commit. The
        // request's captured Core reply operation remains the exact route for
        // the terminal and must get its first submit attempt after the commit.
        await runtime.HandoverAsync();
        operationTarget.Complete(new ActorCreateOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new ActorCreateCompletion(
                ActorCreateResult.Created,
                new ActorRef(
                    operation.ActorId,
                    reservation.ObjectGeneration,
                    "mesh",
                    runtime.TargetRid))));

        var timeout = await Assert.ThrowsAsync<ZlinkRequestException>(
            () => replyTask);
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, timeout.Result);

        var replay = SendActorCreateRequestAsync(runtime.Source, operation);
        var parts = await replay.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(parts, correlation);
            Assert.True(ZLinkServiceWireCodec.TryDecodeActorCreateReply(
                reply,
                "mesh",
                out var completion,
                out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(ActorCreateResult.Created, completion!.Result);
            Assert.Equal("remote-actor", completion.Actor.ActorId);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_PendingReplyUsesTerminalDeadline()
    {
        var attempts = 0;
        var time = new ManualTimeProvider();
        await using var runtime = await ConnectedRuntime.CreateAsync(
            _ =>
            {
                Interlocked.Increment(ref attempts);
                return SubmitResult.Backpressured;
            },
            time);
        await using var monitor = runtime.Target.OpenMonitor();
        var request = CreateRequest(
            correlation: 55,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);

        _ = SendRequestAsync(runtime.Source, request);
        var ingress = await ReceiveActorJoinAsync(runtime.Target);
        Assert.Equal(
            SubmitResult.Backpressured,
            ingress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed));

        time.Advance(TimeSpan.FromSeconds(6));
        await WaitUntilAsync(() => monitor.Status().ProtocolErrors != 0);
        var expiredAttempts = Volatile.Read(ref attempts);
        await Task.Delay(150);
        Assert.Equal(expiredAttempts, Volatile.Read(ref attempts));
        Assert.Equal(
            SubmitResult.InvalidState,
            ingress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));
    }

    [Fact]
    public async Task CanonicalActorJoinRequest_ConcurrentAcceptedAndErrorSubmitOneWire()
    {
        var wireSubmits = 0;
        await using var runtime = await ConnectedRuntime.CreateAsync(reply =>
        {
            reply.Submit();
            Interlocked.Increment(ref wireSubmits);
            return SubmitResult.Ok;
        });
        var request = CreateRequest(
            correlation: 52,
            runtime.SourceRid,
            runtime.TargetRid,
            runtime.Target.Status().LifecycleGeneration,
            runtime.TargetSpotId,
            runtime.TargetSpotGeneration);
        var replyTask = SendRequestAsync(runtime.Source, request);
        var ingress = await ReceiveActorJoinAsync(runtime.Target);
        var start = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var accepted = Task.Run(async () =>
        {
            await start.Task;
            return ingress.ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>());
        });
        var error = Task.Run(async () =>
        {
            await start.Task;
            return ingress.ReplyTerminal(
                RequestResult.InternalError,
                (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed);
        });

        start.SetResult();
        var acceptedResult = await accepted;
        var errorResult = await error;
        var results = new[] { acceptedResult, errorResult };
        Assert.Contains(SubmitResult.Ok, results);
        Assert.Contains(SubmitResult.InvalidState, results);
        Assert.Equal(1, Volatile.Read(ref wireSubmits));

        var parts = await replyTask.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            var reply = DecodeReply(parts, request.Correlation);
            if (acceptedResult == SubmitResult.Ok)
            {
                Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
                    reply,
                    out var completion,
                    out var decodeError));
                Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, decodeError);
                Assert.Equal(ActorJoinResult.Accepted, completion!.JoinResult);
            }
            else
            {
                Assert.Equal(
                    (int)RequestResult.InternalError,
                    reply.TerminalResult);
                Assert.Equal(
                    (uint)ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                    reply.FailureCode);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ActorJoinRequest CreateRequest(
        ulong correlation,
        RoutingId sourceRid,
        RoutingId targetRid,
        ulong targetNodeGeneration,
        string targetSpotId,
        ulong targetSpotGeneration) => new(
        correlation,
        new ActorRef("actor-1", 7, "mesh", sourceRid),
        ActorNodeGeneration: 1,
        ActorAuthorityOwnerGeneration: 3,
        ActorOwnerLeaseGeneration: 5,
        Entry: false,
        targetSpotId,
        targetSpotGeneration,
        targetRid,
        targetNodeGeneration,
        TargetAuthorityOwnerGeneration: 4,
        TargetOwnerLeaseGeneration: 6);

    private static Task<IReadOnlyList<Message>> SendRequestAsync(
        IDealerSocket source,
        ActorJoinRequest request)
    {
        using var head = Message.From(
            ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(request));
        using var payload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.Encode(
                "ZLinkFrameworkActorJoinRequest",
                "application/json",
                "{}"u8));
        return source.Request()
            .Message(head)
            .Message(payload)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async(CancellationToken.None);
    }

    private static Task<IReadOnlyList<Message>> SendActorCreateRequestAsync(
        IDealerSocket source,
        ActorCreateOperation operation)
    {
        using var head = Message.From(
            ZLinkServiceWireCodec.EncodeActorCreate(operation));
        return source.Request()
            .Message(head)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async(CancellationToken.None);
    }

    private static ZLinkServiceWireCodec.ReplyRecord DecodeReply(
        IReadOnlyList<Message> parts,
        ulong correlation)
    {
        Assert.Single(parts);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            parts[0].AsReadOnlyMemory().Span,
            out var reply,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(correlation, reply.Correlation);
        return reply;
    }

    private static async Task<MeshReceiveRecord> ReceiveActorJoinAsync(
        ZLinkManagedMeshNode target,
        TimeSpan? timeout = null)
    {
        var deadline = DateTime.UtcNow + (timeout ?? TimeSpan.FromSeconds(2));
        while (DateTime.UtcNow < deadline)
        {
            using var ready = new MeshReadyBatch();
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            for (var index = 0; index < ready.Count; index++)
            {
                using var claim = ready.TakeClaim(index);
                using var received = new MeshReceiveBatch();
                while (claim.Receive(received, RecvFlags.DontWait))
                {
                    for (var recordIndex = 0;
                         recordIndex < received.Count;
                         recordIndex++)
                    {
                        var record = received[recordIndex];
                        if (record.OperationKind == MeshOperationKind.ActorJoin)
                            return record;
                    }
                    received.Reset();
                }
            }
            await Task.Delay(10);
        }
        throw new TimeoutException("Canonical actorJoin ingress was not queued.");
    }

    private static async Task<(
        MeshReceiveRecord Record,
        IReadOnlyList<Message> Parts)> ReceiveCompletionAsync(
            ZLinkManagedMeshNode source,
            MeshOperationId operationId)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            using var ready = new MeshReadyBatch();
            source.DrainReady(
                MeshReadyDomains.Infrastructure,
                ready,
                RecvFlags.DontWait);
            for (var index = 0; index < ready.Count; index++)
            {
                using var claim = ready.TakeClaim(index);
                using var received = new MeshReceiveBatch();
                while (claim.Receive(received, RecvFlags.DontWait))
                {
                    for (var recordIndex = 0;
                         recordIndex < received.Count;
                         recordIndex++)
                    {
                        var record = received[recordIndex];
                        if (record.Kind == MeshRecordKind.Completion
                            && record.OperationId == operationId)
                            return (record, received.RetainMessage(recordIndex));
                    }
                    received.Reset();
                }
            }
            await Task.Delay(10);
        }
        throw new TimeoutException("Canonical actorJoin completion was not queued.");
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var startedAt = Stopwatch.GetTimestamp();
        while (!condition())
        {
            if (Stopwatch.GetElapsedTime(startedAt) >= TimeSpan.FromSeconds(2))
                throw new TimeoutException("Canonical actorJoin setup did not complete.");
            await Task.Delay(10);
        }
    }

    private static async Task<Received> ReceiveAsync(IDealerSocket source)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            var received = Received.Create();
            if (source.Recv(received, RecvFlags.DontWait))
                return received;
            received.Dispose();
            await Task.Delay(10);
        }
        throw new TimeoutException("Route admission reply was not received.");
    }

    private static async Task<MonitorEvent> ReceiveMonitorEventAsync(
        ISocketMonitor monitor,
        MonitorEventType expected)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            var value = monitor.Recv(RecvFlags.DontWait);
            if (value?.Event == expected)
                return value;
            await Task.Delay(10);
        }
        throw new TimeoutException($"Monitor event '{expected}' was not received.");
    }

    private static async Task SendHelloAsync(
        IDealerSocket source,
        string sourceEndpoint)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (true)
        {
            try
            {
                using var hello = Message.From(
                    ZLinkServiceWireCodec.EncodeRouteAdmission(
                        ServiceWireConstants.Command.Hello,
                        "mesh",
                        sourceEndpoint,
                        lifecycleGeneration: 1,
                        descriptorRevision: 1,
                        new Dictionary<string, uint>(StringComparer.Ordinal),
                        objectRole: (byte)ZLinkMeshNodeObjectRole.Server));
                await source.Send().Message(hello).Async(CancellationToken.None);
                return;
            }
            catch (ZlinkSubmitException) when (DateTime.UtcNow < deadline)
            {
                await Task.Delay(10);
            }
        }
    }

    private static string AllocateTcpEndpoint()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = Assert.IsType<IPEndPoint>(listener.LocalEndpoint);
        return $"tcp://127.0.0.1:{endpoint.Port}";
    }

    private sealed class ConnectedRuntime : IAsyncDisposable
    {
        private ConnectedRuntime(
            IContext context,
            ZLinkManagedMeshNode target,
            IDealerSocket source,
            RoutingId sourceRid,
            RoutingId targetRid,
            string targetSpotId,
            ulong targetSpotGeneration,
            string targetEndpoint,
            string sourceEndpoint)
        {
            Context = context;
            Target = target;
            Source = source;
            SourceRid = sourceRid;
            TargetRid = targetRid;
            TargetSpotId = targetSpotId;
            TargetSpotGeneration = targetSpotGeneration;
            TargetEndpoint = targetEndpoint;
            SourceEndpoint = sourceEndpoint;
            _createdSources.Add(source);
        }

        internal IContext Context { get; }
        internal ZLinkManagedMeshNode Target { get; }
        internal IDealerSocket Source { get; private set; }
        internal RoutingId SourceRid { get; }
        internal RoutingId TargetRid { get; }
        internal string TargetSpotId { get; }
        internal ulong TargetSpotGeneration { get; }
        private string TargetEndpoint { get; }
        private string SourceEndpoint { get; }
        private IDealerSocket? PriorSource { get; set; }
        private readonly List<IDealerSocket> _createdSources = new();

        internal ValueTask DisconnectSourceAsync() => Source.DisposeAsync();

        internal async Task SendIdempotentHelloAsync()
        {
            await SendHelloAsync(Source, SourceEndpoint);
            using var admission = await ReceiveAsync(Source);
        }

        internal async Task ReconnectAsync()
        {
            var replacement = Context.CreateDealerSocket();
            _createdSources.Add(replacement);
            replacement.SetRoutingId(SourceRid);
            replacement.Connect(TargetEndpoint);
            Source = replacement;
            await SendHelloAsync(replacement, SourceEndpoint);
            using var admission = await ReceiveAsync(replacement);
        }

        internal async Task HandoverAsync(
            Func<IDealerSocket, string, Task<Received>>? admitReplacement = null)
        {
            var replacement = Context.CreateDealerSocket();
            _createdSources.Add(replacement);
            replacement.SetRoutingId(SourceRid);
            replacement.Connect(TargetEndpoint);
            if (admitReplacement is null)
            {
                await SendHelloAsync(replacement, SourceEndpoint);
                using var admission = await ReceiveAsync(replacement);
            }
            else
            {
                using var admission = await admitReplacement(
                    replacement,
                    SourceEndpoint);
            }
            PriorSource = Source;
            Source = replacement;
        }

        internal Task SendPriorHelloAsync()
        {
            if (PriorSource is not { } prior)
                throw new InvalidOperationException("No prior source is available.");
            return SendHelloAsync(prior, SourceEndpoint);
        }

        internal async Task SendPriorHelloAndDisconnectAsync()
        {
            if (PriorSource is not { } prior)
                throw new InvalidOperationException("No prior source is available.");
            PriorSource = null;
            try
            {
                await SendHelloAsync(prior, SourceEndpoint);
            }
            finally
            {
                await prior.DisposeAsync();
            }
        }

        internal async ValueTask DisconnectPriorSourceAsync()
        {
            if (PriorSource is not { } prior)
                throw new InvalidOperationException("No prior source is available.");
            PriorSource = null;
            await prior.DisposeAsync();
        }

        internal static async Task<ConnectedRuntime> CreateAsync(
            Func<ReplySubmitOperation, SubmitResult> submit,
            TimeProvider? deadlineTimeProvider = null)
        {
            var context = Systems.Zlink.Zlink.CreateContext();
            var target = new ZLinkManagedMeshNode(
                context,
                "mesh",
                deadlineTimeProvider: deadlineTimeProvider,
                nativeTerminalReplySubmitOverride: submit);
            var suffix = Guid.NewGuid().ToString("N");
            var sourceRid = RoutingId.From($"actor-join-source-{suffix}");
            var targetRid = RoutingId.From($"actor-join-target-{suffix}");
            var targetEndpoint = AllocateTcpEndpoint();
            var sourceEndpoint = $"inproc://actor-join-source-{suffix}";
            const string targetSpotId = "target-spot";

            target.SetRoutingId(targetRid);
            target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
            target.SetBind(targetEndpoint);
            var targetSpot = target.GetOrCreateSpot(
                targetSpotId,
                out var created);
            Assert.True(created);
            target.Start();

            var source = context.CreateDealerSocket();
            source.SetRoutingId(sourceRid);
            source.Connect(targetEndpoint);
            await SendHelloAsync(source, sourceEndpoint);

            await WaitUntilAsync(() =>
                target.Status().AdmittedPeerCount == 1);
            using var admission = await ReceiveAsync(source);
            return new ConnectedRuntime(
                context,
                target,
                source,
                sourceRid,
                targetRid,
                targetSpotId,
                targetSpot.LifecycleGeneration,
                targetEndpoint,
                sourceEndpoint);
        }

        public async ValueTask DisposeAsync()
        {
            foreach (var source in _createdSources)
                await source.DisposeAsync();
            await Target.DisposeAsync();
            await Context.DisposeAsync();
        }
    }

    private sealed class DeferredActorCreateOperationTarget
        : IActorCreateOperationTarget
    {
        private readonly TaskCompletionSource _started = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource<ActorCreateOperationTerminal> _terminal =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal Task Started => _started.Task;

        public async ValueTask<ActorCreateOperationTerminal> CreateAsync(
            ActorCreateOperation operation,
            CancellationToken cancellationToken)
        {
            _ = operation;
            _started.TrySetResult();
            return await _terminal.Task.WaitAsync(cancellationToken);
        }

        internal void Complete(ActorCreateOperationTerminal terminal) =>
            _terminal.TrySetResult(terminal);
    }
}
