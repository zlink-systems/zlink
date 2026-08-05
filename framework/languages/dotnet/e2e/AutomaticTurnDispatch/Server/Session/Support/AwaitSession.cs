using Systems.Zlink;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace AutomaticTurnDispatch.Server.Session.Support;

internal sealed partial class AwaitSession(
    IZLinkSessionContext context,
    IZLinkRouteClient routes,
    IZLinkRouteMeshRuntime meshRuntime,
    IZLinkSpotClient spotsClient,
    EvidenceStore evidence) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-connected|rid={evidence.Rid}|session={Context.SessionId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-disconnected|rid={evidence.Rid}|session={Context.SessionId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-error|rid={evidence.Rid}|session={Context.SessionId}|error={error.Error}");
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        switch (dispatch.PacketName)
        {
            case "BindAwaitActorsReq":
            {
                var request = payload.Decode<BindAwaitActorsReq>();
                evidence.Add(
                    $"session-bind-actors|rid={evidence.Rid}|session={Context.SessionId}|spot={request.SpotRid}");
                var result = await RequestPlayControlAsync<BindAwaitActorsRes>(
                    routes,
                    request,
                    cancellationToken);
                foreach (var actor in result.Actors)
                {
                    await Context.Actors.BindAsync(
                        new ActorRef(
                            actor.ActorId,
                            actor.Generation,
                            AutomaticTurnDispatchNames.SpotChannel,
                            RoutingId.From(actor.NodeRid)),
                        cancellationToken);
                    evidence.Add(
                        $"session-bound-actor|rid={evidence.Rid}|session={Context.SessionId}"
                        + $"|actor={actor.ActorId}|node={actor.NodeRid}");
                }

                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "AwaitShutdownScenarioReq":
            {
                var request = payload.Decode<AwaitShutdownScenarioReq>();
                evidence.Add(
                    $"session-shutdown|rid={evidence.Rid}|session={Context.SessionId}|request={request.RequestId}|spot={request.SpotRid}");
                var result = await RunShutdownThroughSpotRouteAsync(
                    routes,
                    spotsClient,
                    request,
                    cancellationToken);
                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "AwaitShutdownRecoveryReq":
            {
                var request = payload.Decode<AwaitShutdownRecoveryReq>();
                evidence.Add(
                    $"session-shutdown-recovery|rid={evidence.Rid}|session={Context.SessionId}|request={request.RequestId}|spot={request.SpotRid}");
                var result = await RunShutdownRecoveryThroughSpotRouteAsync(
                    routes,
                    spotsClient,
                    request,
                    cancellationToken);
                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "AwaitEvidenceReq":
            {
                var request = payload.Decode<AwaitEvidenceReq>();
                var result = await RequestPlayControlAsync<AwaitEvidenceRes>(
                    routes,
                    request,
                    TargetOrDefault(dispatch),
                    cancellationToken);
                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "AwaitEvidenceWaitReq":
            {
                var request = payload.Decode<AwaitEvidenceWaitReq>();
                var result = await RequestPlayControlAsync<AwaitEvidenceRes>(
                    routes,
                    request,
                    TargetOrDefault(dispatch),
                    cancellationToken);
                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "EnsureSpotReq":
            {
                var request = payload.Decode<EnsureSpotReq>();
                var result = await RequestPlayControlAsync<EnsureSpotRes>(
                    routes,
                    request,
                    TargetOrDefault(dispatch),
                    cancellationToken);
                await Context.Client.Reply(result).Async(cancellationToken);
                return;
            }
            case "HoldReq":
            {
                await ReplySpotRequestAsync<HoldReq, AutomaticTurnDispatchRes>(dispatch, payload, cancellationToken);
                return;
            }
            case "AwaitReq":
            {
                await ReplySpotRequestAsync<AwaitReq, AutomaticTurnDispatchRes>(dispatch, payload, cancellationToken);
                return;
            }
            case "WorkerAwaitReq":
            {
                await ReplySpotRequestAsync<WorkerAwaitReq, AutomaticTurnDispatchRes>(dispatch, payload, cancellationToken);
                return;
            }
            case "RemoteSpotAwaitReq":
            {
                await ReplySpotRequestAsync<RemoteSpotAwaitReq, AutomaticTurnDispatchRes>(dispatch, payload,
                    cancellationToken);
                return;
            }
            case "ProbeReq":
            {
                await ReplySpotRequestAsync<ProbeReq, AutomaticTurnDispatchRes>(dispatch, payload, cancellationToken);
                return;
            }
            case "HoldMsg":
            {
                await RelaySpotCommandAsync<HoldMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "AwaitMsg":
            {
                await RelaySpotCommandAsync<AwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "CounterResetMsg":
            {
                await RelaySpotCommandAsync<CounterResetMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "CounterAwaitMsg":
            {
                await RelaySpotCommandAsync<CounterAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "CounterReadReq":
            {
                await ReplySpotRequestAsync<CounterReadReq, CounterReadRes>(dispatch, payload, cancellationToken);
                return;
            }
            case "HttpAwaitMsg":
            {
                await RelaySpotCommandAsync<HttpAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "IoWorkerAwaitMsg":
            {
                await RelaySpotCommandAsync<IoWorkerAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "CpuWorkerAwaitMsg":
            {
                await RelaySpotCommandAsync<CpuWorkerAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "SelfCycleMsg":
            {
                await RelaySpotCommandAsync<SelfCycleMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "SelfSendMsg":
            {
                await RelaySpotCommandAsync<SelfSendMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "DeferredJoinFailureMsg":
            {
                await RelaySpotCommandAsync<DeferredJoinFailureMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "WorkerAwaitMsg":
            {
                await RelaySpotCommandAsync<WorkerAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "RemoteSpotAwaitMsg":
            {
                await RelaySpotCommandAsync<RemoteSpotAwaitMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "ProbeMsg":
            {
                await RelaySpotCommandAsync<ProbeMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "AwaitTimeoutMsg":
            {
                await RelaySpotCommandAsync<AwaitTimeoutMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "AwaitCancelMsg":
            {
                await RelaySpotCommandAsync<AwaitCancelMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "TimerStartMsg":
            {
                await RelaySpotCommandAsync<TimerStartMsg>(dispatch, payload, cancellationToken);
                return;
            }
            case "TimerStopMsg":
            {
                await RelaySpotCommandAsync<TimerStopMsg>(dispatch, payload, cancellationToken);
                return;
            }
            default:
            {
                var actorId = dispatch.Metadata.Find(AutomaticTurnDispatchNames.ActorIdMetadata);
                var actor = string.IsNullOrWhiteSpace(actorId)
                    ? RequireSingleBoundActor()
                    : Context.Actors.Find(actorId);
                if (actor is null)
                {
                    throw new InvalidOperationException($"Actor route not found: {actorId}");
                }

                await actor.RelayAsync(payload, cancellationToken);
                return;
            }
        }
    }

    private IZLinkSessionActor RequireSingleBoundActor()
    {
        return Context.Actors.Bound.Count switch
        {
            1 => Context.Actors.Bound.Single(),
            0 => throw new InvalidOperationException("No actor is bound."),
            _ => throw new InvalidOperationException("actor-id metadata is required.")
        };
    }

    private async Task ReplySpotRequestAsync<TReq, TRes>(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        var spotRid = dispatch.Metadata.Find(AutomaticTurnDispatchNames.SpotRidMetadata);
        if (string.IsNullOrWhiteSpace(spotRid))
            throw new InvalidOperationException($"{AutomaticTurnDispatchNames.SpotRidMetadata} metadata is required.");

        var request = payload.Decode<TReq>()
                      ?? throw new InvalidOperationException($"Failed to decode packet '{dispatch.PacketName}'.");
        var result = await RequestSpotAsync<TRes>(
            spotsClient,
            spotRid,
            request,
            cancellationToken);
        await Context.Client.Reply(result).Async(cancellationToken);
    }

    private async Task RelaySpotCommandAsync<TMsg>(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        var spotRid = dispatch.Metadata.Find(AutomaticTurnDispatchNames.SpotRidMetadata);
        if (string.IsNullOrWhiteSpace(spotRid))
            throw new InvalidOperationException($"{AutomaticTurnDispatchNames.SpotRidMetadata} metadata is required.");

        var command = payload.Decode<TMsg>()
                      ?? throw new InvalidOperationException($"Failed to decode packet '{dispatch.PacketName}'.");
        await SendSpotAsync(
            spotsClient,
            spotRid,
            command,
            cancellationToken);
    }
}
