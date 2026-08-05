using ChannelEgressRouting.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace ChannelEgressRouting.Server;

internal sealed class ChannelBindActorHandler(
    IZLinkActorManager actors)
    : IZLinkSessionPacketHandler<
        IZLinkSessionContext,
        ChannelBindActorRequest>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        ChannelBindActorRequest request,
        CancellationToken cancellationToken)
    {
        var actor = await actors.FindAsync(request.ActorId, cancellationToken)
                    ?? throw new InvalidOperationException(
                        $"Actor '{request.ActorId}' was not found.");
        await context.Actors.BindOrGetAsync(actor, cancellationToken);
        await context.Client.Reply(new ChannelBindActorReply(
                actor.ActorId,
                actor.NodeRid.ToString(),
                actor.ObjectGeneration))
            .Async(cancellationToken);
    }
}

internal sealed class ChannelSession(
    IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddHandler<ChannelBindActorHandler>();

    public ValueTask OnConnectedAsync(
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(
        CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(
                dispatch,
                payload,
                cancellationToken))
            return;
        var actor = Context.Actors.Bound.Count == 1
            ? Context.Actors.Bound.Single()
            : throw new InvalidOperationException(
                "A relayed packet requires exactly one bound Actor.");
        await actor.RelayAsync(payload, cancellationToken);
    }
}
