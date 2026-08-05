using SpotActorTransfer.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace SpotActorTransfer.SessionGateway;

internal sealed class BindActorSessionHandler(
    IZLinkActorManager actors,
    GatewayEvidenceStore evidence) : IZLinkSessionPacketHandler<IZLinkSessionContext, BindActorSessionReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        BindActorSessionReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var resolved = request.NodeRid is not null && request.Generation is not null
            ? new ActorRef(
                request.ActorId,
                checked((ulong)request.Generation.Value),
                SpotActorTransferNames.Mesh,
                RoutingId.From(request.NodeRid))
            : await actors.FindAsync(request.ActorId, cancellationToken)
              ?? throw new InvalidOperationException(
                  $"Actor '{request.ActorId}' was not found.");
        _ = await context.Actors.BindOrGetAsync(resolved, cancellationToken).ConfigureAwait(false);
        evidence.Add(request.Scenario, request.ActorId, "session_bound", context.SessionId);
        await context.Client.Reply(new BindActorSessionRes(
                request.Scenario, resolved.ActorId, resolved.NodeRid.ToString(), checked((long)resolved.ObjectGeneration)))
            .Async(cancellationToken);
    }
}

internal sealed class SessionBindingsHandler
    : IZLinkSessionPacketHandler<IZLinkSessionContext, SessionBindingsReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        SessionBindingsReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        await context.Client.Reply(new SessionBindingsRes(
                request.Scenario,
                context.Actors.Bound
                    .Select(actor => new SessionBindingSnapshot(
                        actor.ActorId,
                        actor.Ref.NodeRid.ToString(),
                        checked((long)actor.Ref.ObjectGeneration)))
                    .OrderBy(static actor => actor.ActorId, StringComparer.Ordinal)
                    .ToArray()))
            .Async(cancellationToken);
    }
}

internal sealed class TransferSession(
    IZLinkSessionContext context,
    GatewayEvidenceStore evidence) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<BindActorSessionHandler>();
        Context.Handlers.AddHandler<SessionBindingsHandler>();
    }

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add("session", Context.SessionId, "connected", Context.RoutingId?.ToString() ?? "");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add("session", Context.SessionId, "disconnected", Context.RoutingId?.ToString() ?? "");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add("session", Context.SessionId, "error", error.ToString());
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken).ConfigureAwait(false)) return;
        var targetActorId = payload.Decode<BoundPushReq>().ActorId;
        var actor = targetActorId is null
            ? Context.Actors.Bound.SingleOrDefault()
            : Context.Actors.Bound.SingleOrDefault(bound =>
                string.Equals(
                    bound.ActorId,
                    targetActorId,
                    StringComparison.Ordinal));
        if (actor is null)
            throw new InvalidOperationException(
                targetActorId is null
                    ? "No actor is bound."
                    : $"Actor '{targetActorId}' is not bound.");
        await actor.RelayAsync(payload, cancellationToken).ConfigureAwait(false);
    }
}
