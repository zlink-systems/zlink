using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Domain.SupportChat;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Notifications;

// Adapter: turns pure domain conversation events into stream push messages and sends
// them to the participants' bound sessions (§8). Domain snapshots/messages are mapped
// to wire contracts through ConversationContracts.
internal sealed class ConversationNotificationPublisher
{
    public async ValueTask PublishAsync(
        IReadOnlyList<ConversationEvent> events,
        IReadOnlyDictionary<string, SupportUserActor> actors,
        CancellationToken cancellationToken)
    {
        foreach (var conversationEvent in events) await PublishAsync(conversationEvent, actors, cancellationToken);
    }

    // Sent when a conversation is assigned to an agent, before the agent joins. It goes
    // to the agent's roster actor so the agent client knows which conversation to join.
    public async ValueTask PublishAssignedToRosterAsync(
        SupportUserActor roster,
        ConversationSnapshot snapshot,
        CancellationToken cancellationToken)
    {
        var state = ConversationContracts.ToState(snapshot);
        await roster.Context.BoundSession
            .Send(new ConversationAssignedNotify(state.ConversationId, state))
            .Async(cancellationToken);
    }

    private async ValueTask PublishAsync(
        ConversationEvent conversationEvent,
        IReadOnlyDictionary<string, SupportUserActor> actors,
        CancellationToken cancellationToken)
    {
        var state = ConversationContracts.ToState(conversationEvent.State);
        switch (conversationEvent.Kind)
        {
            case ConversationEventKind.ParticipantJoined:
                await PublishParticipantJoinedAsync(conversationEvent, state, actors, cancellationToken);
                break;
            case ConversationEventKind.MessageAppended:
                await PublishMessageAsync(conversationEvent, state, actors, cancellationToken);
                break;
            case ConversationEventKind.TypingChanged:
                await PublishTypingAsync(conversationEvent, state, actors, cancellationToken);
                break;
            case ConversationEventKind.Idle:
                await PublishAllAsync(
                    actors,
                    async actor =>
                    {
                        await actor.Context.BoundSession
                            .Send(new ConversationIdleNotify(state.ConversationId, state))
                            .Async(cancellationToken);
                    });
                break;
            case ConversationEventKind.Closed:
                // An explicit close carries the requester's participant id: that client
                // already gets the closed state in CloseConversationRes, so only the
                // other participant is notified. An auto-close (idle grace) has no
                // requester, so both participants are notified.
                await PublishAllAsync(
                    conversationEvent.ActorId is null
                        ? actors
                        : Exclude(actors, conversationEvent.ActorId),
                    async actor =>
                    {
                        await actor.Context.BoundSession
                            .Send(new ConversationClosedNotify(state.ConversationId, state))
                            .Async(cancellationToken);
                    });
                break;
            default:
                throw new InvalidOperationException($"Unsupported conversation event {conversationEvent.Kind}.");
        }
    }

    private static async ValueTask PublishParticipantJoinedAsync(
        ConversationEvent conversationEvent,
        ConversationState state,
        IReadOnlyDictionary<string, SupportUserActor> actors,
        CancellationToken cancellationToken)
    {
        if (conversationEvent.ActorId is null || conversationEvent.Role is null)
            throw new InvalidOperationException("Participant joined event requires actor id and role.");

        // Membership commit is observable by every participant, including the
        // joining agent. The deferred Join reply only reports scheduling.
        await PublishAllAsync(
            actors,
            async actor =>
            {
                await actor.Context.BoundSession
                    .Send(new ParticipantJoinedNotify(
                        state.ConversationId,
                        conversationEvent.ActorId,
                        ConversationContracts.ToRole(conversationEvent.Role.Value),
                        state))
                    .Async(cancellationToken);
            });
    }

    private static async ValueTask PublishMessageAsync(
        ConversationEvent conversationEvent,
        ConversationState state,
        IReadOnlyDictionary<string, SupportUserActor> actors,
        CancellationToken cancellationToken)
    {
        var message = conversationEvent.Message
                      ?? throw new InvalidOperationException("Message event requires a chat message.");
        var chatMessage = ConversationContracts.ToMessage(message);
        await PublishAllAsync(
            Exclude(actors, message.SenderActorId),
            async actor =>
            {
                await actor.Context.BoundSession
                    .Send(new ChatMessageNotify(state.ConversationId, chatMessage, state))
                    .Async(cancellationToken);
            });
    }

    private static async ValueTask PublishTypingAsync(
        ConversationEvent conversationEvent,
        ConversationState state,
        IReadOnlyDictionary<string, SupportUserActor> actors,
        CancellationToken cancellationToken)
    {
        if (conversationEvent.ActorId is null || conversationEvent.IsTyping is null)
            throw new InvalidOperationException("Typing event requires actor id and typing state.");

        await PublishAllAsync(
            Exclude(actors, conversationEvent.ActorId),
            async actor =>
            {
                await actor.Context.BoundSession
                    .Send(new TypingChangedNotify(
                        state.ConversationId,
                        conversationEvent.ActorId,
                        conversationEvent.IsTyping.Value,
                        state))
                    .Async(cancellationToken);
            });
    }

    private static IReadOnlyDictionary<string, SupportUserActor> Exclude(
        IReadOnlyDictionary<string, SupportUserActor> actors,
        string participantId)
    {
        return actors
            .Where(actor => !string.Equals(actor.Key, participantId, StringComparison.Ordinal))
            .ToDictionary(static actor => actor.Key, static actor => actor.Value, StringComparer.Ordinal);
    }

    private static async ValueTask PublishAllAsync(
        IReadOnlyDictionary<string, SupportUserActor> actors,
        Func<SupportUserActor, ValueTask> publish)
    {
        foreach (var actor in actors.Values) await publish(actor);
    }
}
