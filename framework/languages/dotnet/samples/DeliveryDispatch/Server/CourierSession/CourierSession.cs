using Zlink.Framework.Contracts.Streams;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;

namespace DeliveryDispatch.Server.CourierSession;

internal sealed class CourierSession(
    IZLinkSessionContext context,
    ILogger<CourierSession> logger) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "deliverydispatch courier-session: connected session={SessionId}",
            Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        foreach (var actor in Context.Actors.Bound)
        {
            await actor.NotifyDisconnectedAsync(cancellationToken);
        }
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        logger.LogError(
            "deliverydispatch courier-session: stream error code={Code} message={Message} session={SessionId}",
            error.Error,
            error.Message,
            Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "deliverydispatch courier-session: dispatch packet={PacketName} session={SessionId}",
            dispatch.PacketName,
            Context.SessionId);
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
        {
            return;
        }

        var decision = payload.Decode<CourierDecisionMsg>();
        var actor = Context.Actors.Find(decision.CourierId);
        if (actor is null)
        {
            throw new InvalidOperationException($"Courier actor is not bound: {decision.CourierId}");
        }

        await actor.RelayAsync(payload, cancellationToken);
    }
}
