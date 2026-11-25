using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkActorHandoffIngress
{
    internal static bool CanAdmitStaleManagedIngress(
        ZLinkFrameworkRuntime runtime,
        ActorMessageFollowIngress ingress
    )
    {
        if (!runtime.TryGetActorState(ingress.TargetActor.ActorId, out var state))
            return false;
        var actor = new ZLinkBackendActorRef(
            ingress.TargetActor.NodeRid,
            ingress.TargetActor.ActorId,
            ingress.TargetActor.ObjectGeneration
        );
        var route = new ZLinkBackendActorRouteContext(
            ingress.OperationId,
            ingress.MessageFollowHopCount,
            ingress.TargetNodeGeneration,
            ingress.AuthorityOwnerGeneration,
            ingress.OwnerLeaseGeneration,
            ingress.ReplyRouteId,
            ingress.Reply is null ? 0u : 1u,
            DeadlineUnixMs: ingress.DeadlineUnixMs
        );
        return ingress.EarlyAdmission is not null
            || state.Handoff.CapturesSourceIngress
            || ZLinkActorMessageFollowDispatcher.CanFollow(state, actor, route);
    }

    internal static bool TryCaptureOrFollowStaleManagedIngress(
        ZLinkFrameworkRuntime runtime,
        IReadOnlyList<ZLinkBackendActorPart> parts,
        string receiverMeshName,
        ActorMessageFollowIngress? ingress = null
    )
    {
        if (parts.Count == 0)
            return false;
        var headerPart = parts[0];
        if (!runtime.TryGetActorState(headerPart.Actor.ActorId, out var state))
            return false;
        var earlyAdmission = ingress?.EarlyAdmission;
        var target = earlyAdmission is null
            ? null
            : runtime.Services.GetService<ZLinkSpotRetireTargetRuntime>();
        var stage = ingress is { } candidate
            ? target?.FindActorAdmissionStage(state, candidate)
            : null;
        if (
            stage is null
            && (
                earlyAdmission is not null && headerPart.SourceNodeRid == headerPart.Actor.NodeRid
                || !state.Handoff.CapturesSourceIngress
                    && !ZLinkActorMessageFollowDispatcher.CanFollow(
                        state,
                        headerPart.Actor,
                        headerPart.RouteContext
                    )
            )
        )
            return false;

        var index = 1;
        if (
            !ZLinkSpotActorFrameReader.TryRead(
                earlyAdmission is not null && headerPart.SourceNodeRid == headerPart.Actor.NodeRid
                    ? ZLinkApplicationJobOrigin.Local
                    : ZLinkApplicationJobOrigin.Remote,
                parts,
                ref index,
                headerPart,
                runtime.Flow.CaptureEnabled,
                out var frame
            )
        )
        {
            if (stage is not null)
                earlyAdmission!.Reject();
            return true;
        }
        var frameTransferred = false;
        try
        {
            frame.AcceptedState = state;
            if (stage is not null)
            {
                var admission = earlyAdmission!;
                // Reserve in arrival order. The Store check completes outside the
                // state lane, before replay may fill this frame's queue position.
                var authorization = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                frame.TargetAdmission = authorization.Task;
                if (
                    frame.RequestSource is { } source
                    && state.Handoff.TryCaptureTargetRequest(
                        stage.Envelope.AggregateId.ToString("N"),
                        frame,
                        runtime
                    ) == ZLinkActorHandoffCaptureResult.Captured
                )
                {
                    _ = ConfirmTargetRequestAsync(
                        target!,
                        stage,
                        frame.Actor,
                        frame.RouteContext.AuthorityOwnerGeneration,
                        source,
                        admission,
                        authorization,
                        runtime
                    );
                    return true;
                }

                if (!admission.IsCurrent())
                {
                    admission.Reject();
                    return true;
                }

                var ready = new ZLinkSpotActorFrameBatch([frame]);
                frameTransferred = TryDispatch(runtime, receiverMeshName, ready);
                if (frameTransferred)
                    admission.OnAccepted?.Invoke();
                else
                    admission.Reject();
                return true;
            }

            var capture = state.Handoff.TryCapture(
                frame,
                runtime,
                static (rt, fr) => ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(rt, fr)
            );
            if (capture == ZLinkActorHandoffCaptureResult.Captured)
                return true;

            try
            {
                if (
                    ZLinkActorMessageFollowDispatcher.TryFollow(
                        runtime,
                        state,
                        frame.Actor,
                        frame.SourceNodeRid,
                        frame.SourceSessionRid,
                        frame.RequestId,
                        frame.Flags,
                        frame.RouteContext,
                        frame.Header,
                        frame.Body,
                        frame.SourceNodeGeneration,
                        frame.RequestSource,
                        frame.DirectReply,
                        frame.ApplicationMetadata
                    )
                )
                    return true;
            }
            catch (ZLinkFrameworkException exception)
                when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
            {
                // The regular inbound pipeline returns the typed moving/stale
                // terminal after a capture-to-follow transition race.
            }

            var owned = new ZLinkSpotActorFrameBatch([frame]);
            frameTransferred = true;
            if (!TryDispatch(runtime, receiverMeshName, owned))
                owned.Dispose();
            return true;
        }
        finally
        {
            if (!frameTransferred)
                frame.Dispose();
            for (; index < parts.Count; index++)
                parts[index].Message.Dispose();
        }
    }

    private static bool TryDispatch(
        ZLinkFrameworkRuntime runtime,
        string receiverMeshName,
        ZLinkSpotActorFrameBatch batch
    ) =>
        runtime.TryRunDetached(
            "actor-stale-managed-ingress",
            token =>
                new ZLinkActorInboundPipeline(
                    runtime,
                    new ZLinkEntrySpotActorInboundEndpoint(runtime),
                    receiverMeshName
                ).DispatchAsync(batch, token)
        );

    private static async Task ConfirmTargetRequestAsync(
        ZLinkSpotRetireTargetRuntime target,
        TargetStage stage,
        ZLinkBackendActorRef actor,
        ulong authorityOwnerGeneration,
        ZLinkServiceWireCodec.RequestSourceFence source,
        EarlyActorRequestAdmission admission,
        TaskCompletionSource<bool> authorization,
        ZLinkFrameworkRuntime runtime
    )
    {
        var accepted = false;
        try
        {
            accepted =
                await target
                    .MatchesAdmissionAuthorityAsync(
                        stage,
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId),
                        authorityOwnerGeneration,
                        admission.CancellationToken
                    )
                    .ConfigureAwait(false) || admission.IsCurrent();
            if (accepted)
                accepted =
                    await ZLinkRequestSourceFenceResolver
                        .AcceptOrUnavailableAsync(
                            stage.Node.Node,
                            source.NodeRid,
                            source.NodeGeneration,
                            admission.CancellationToken
                        )
                        .ConfigureAwait(false) == source;
            if (accepted)
                admission.OnAccepted?.Invoke();
            else
                admission.Reject();
        }
        catch (OperationCanceledException)
            when (admission.CancellationToken.IsCancellationRequested)
        {
            accepted = false;
            admission.Reject();
        }
        catch (Exception error)
        {
            accepted = false;
            runtime.ErrorSink.ReportRuntimeTaskException("actor-target-admission", error);
            admission.Reject();
        }
        finally
        {
            authorization.TrySetResult(accepted);
        }
    }

    public static ZLinkSpotActorFrameBatch CaptureMovingFrames(
        ZLinkApplicationJobOrigin origin,
        ZLinkFrameworkRuntime runtime,
        IReadOnlyList<ZLinkBackendActorPart> parts,
        IDisposable? payloadOwner = null
    )
    {
        var dispatchable = new List<ZLinkSpotActorFrame>(parts.Count / 2);
        var index = 0;
        while (index < parts.Count)
        {
            var headerPart = parts[index++];
            if (
                !ZLinkSpotActorFrameReader.TryRead(
                    origin,
                    parts,
                    ref index,
                    headerPart,
                    runtime.Flow.CaptureEnabled,
                    out var frame
                )
            )
            {
                continue;
            }
            // An unaccepted frame is not captured; the inbound pipeline ends it Unavailable.
            if (ZLinkRequestSourceFenceResolver.LacksRecordedSource(runtime, frame.RequestSource))
            {
                dispatchable.Add(frame);
                continue;
            }
            try
            {
                var state = runtime.GetOrCreateActorState(frame.Actor.ActorId);
                frame.AcceptedState = state;
                var ingress = state.Handoff.ProjectArrivalIngress(
                    state.NativeActorRef,
                    frame,
                    runtime,
                    static (rt, fr) => ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(rt, fr)
                );
                var capture = ingress.Capture;
                try
                {
                    if (
                        ZLinkActorMessageFollowDispatcher.TryFollow(
                            runtime,
                            state,
                            ingress.Route,
                            frame.Actor,
                            frame.SourceNodeRid,
                            frame.SourceSessionRid,
                            frame.RequestId,
                            frame.Flags,
                            frame.RouteContext,
                            frame.Header,
                            frame.Body,
                            frame.SourceNodeGeneration,
                            frame.RequestSource,
                            frame.DirectReply,
                            frame.ApplicationMetadata
                        )
                    )
                    {
                        frame.Dispose();
                        continue;
                    }
                }
                catch (ZLinkFrameworkException exception)
                    when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
                {
                    // A Message Follow resolution is projected before capture
                    // so a valid relay is never backlogged. If validation
                    // rejects that route, preserve the previous pump-order
                    // fallback capture before the detached dispatch batch.
                    if (ingress.Route.Route == ZLinkActorFrameRoute.MessageFollow)
                        capture = state.Handoff.TryCapture(
                            frame,
                            runtime,
                            static (rt, fr) =>
                                ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(rt, fr)
                        );
                }

                // Capture here, in pump-event order: the dispatch batches run
                // detached and concurrently, so capturing inside the pipeline
                // would race sibling frames and break the backlog's arrival
                // sequence (spec 23 §10.2).
                if (capture == ZLinkActorHandoffCaptureResult.Captured)
                {
                    // TryCapture copied the payload into the durable handoff
                    // aggregate. The raw Message no longer owns any bytes for
                    // this child frame; the returned batch still owns the
                    // physical record envelope until all sibling frames finish.
                    frame.Dispose();
                    continue;
                }

                dispatchable.Add(frame);
            }
            catch
            {
                try
                {
                    frame.Dispose();
                    foreach (var dispatchableFrame in dispatchable)
                        dispatchableFrame.Dispose();
                    for (; index < parts.Count; index++)
                        parts[index].Message.Dispose();
                }
                finally
                {
                    // No batch was returned, so this method remains the
                    // physical record payload owner on the abort path.
                    payloadOwner?.Dispose();
                }
                throw;
            }
        }

        return new ZLinkSpotActorFrameBatch(dispatchable, payloadOwner: payloadOwner);
    }
}
