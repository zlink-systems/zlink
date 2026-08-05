using SupportChat.Server.Configuration;
using SupportChat.Shared.Contracts;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Actors;

internal sealed class SupportUserActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    private readonly Queue<(string ConversationId, bool NotifyBoundSession)> _pendingJoins = new();
    private readonly HashSet<string> _completedJoinOperations = new(StringComparer.Ordinal);

    public string DisplayName { get; private set; } = actorId;

    public string Role { get; private set; } = string.Empty;

    // Conversation-domain identity: the customer's ActorId, or the agent's roster id
    // for a conversation agent actor. Defaults to ActorId.
    public string ParticipantId { get; private set; } = actorId;

    public string ConversationId { get; private set; } = string.Empty;
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public ActorRef? CurrentRef { get; private set; }

    public void SetIdentity(string displayName, string role, string participantId)
    {
        DisplayName = displayName;
        Role = role;
        ParticipantId = participantId;
    }

    public void JoinConversation(string conversationId)
    {
        ConversationId = conversationId;
    }

    public void TrackDeferredJoin(string conversationId, bool notifyBoundSession)
    {
        _pendingJoins.Enqueue((conversationId, notifyBoundSession));
    }

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operationId = completion switch
        {
            ZLinkActorJoinCompletion.Accepted value => value.OperationId,
            ZLinkActorJoinCompletion.Rejected value => value.OperationId,
            ZLinkActorJoinCompletion.Failed value => value.OperationId,
            _ => throw new InvalidOperationException("Unknown Actor Join completion.")
        };
        var operationKey = operationId.ToString();
        if (_completedJoinOperations.Contains(operationKey))
            return;
        var hasPendingIntent = _pendingJoins.Count > 0;
        var pending = hasPendingIntent
            ? _pendingJoins.Peek()
            : ResolveRecoveredJoin(completion);

        switch (completion)
        {
            case ZLinkActorJoinCompletion.Accepted accepted:
                CurrentRef = accepted.Actor;
                JoinConversation(pending.ConversationId);
                if (hasPendingIntent) _pendingJoins.Dequeue();
                _completedJoinOperations.Add(operationKey);
                return;

            case ZLinkActorJoinCompletion.Rejected:
                if (pending.NotifyBoundSession)
                {
                    await Context.BoundSession
                        .Send(new JoinConversationFailedNotify(
                            pending.ConversationId,
                            "Rejected"))
                        .Metadata(
                            SampleNames.ConversationIdMetadataKey,
                            pending.ConversationId)
                        .Async(cancellationToken);
                }
                if (hasPendingIntent) _pendingJoins.Dequeue();
                _completedJoinOperations.Add(operationKey);
                return;

            case ZLinkActorJoinCompletion.Failed failed:
                if (pending.NotifyBoundSession)
                {
                    await Context.BoundSession
                        .Send(new JoinConversationFailedNotify(
                            pending.ConversationId,
                            failed.Kind.ToString()))
                        .Metadata(
                            SampleNames.ConversationIdMetadataKey,
                            pending.ConversationId)
                        .Async(cancellationToken);
                }
                if (hasPendingIntent) _pendingJoins.Dequeue();
                _completedJoinOperations.Add(operationKey);
                return;
        }
    }

    internal string[] CaptureCompletedJoinOperations() =>
        _completedJoinOperations.ToArray();

    internal void RestoreCompletedJoinOperations(IEnumerable<string> operationIds)
    {
        foreach (var operationId in operationIds)
            _completedJoinOperations.Add(operationId);
    }

    private (string ConversationId, bool NotifyBoundSession) ResolveRecoveredJoin(
        ZLinkActorJoinCompletion completion)
    {
        var reply = completion switch
        {
            ZLinkActorJoinCompletion.Accepted { Reply: { } value } => value,
            ZLinkActorJoinCompletion.Rejected { Reply: { } value } => value,
            _ => throw new InvalidOperationException(
                $"Recovered Actor Join completion has no reply. actor={ActorId}")
        };
        var state = reply.Decode<JoinConversationRes>().State;
        return (
            state.ConversationId,
            string.Equals(Role, SupportChatRoles.Agent, StringComparison.Ordinal));
    }
}
