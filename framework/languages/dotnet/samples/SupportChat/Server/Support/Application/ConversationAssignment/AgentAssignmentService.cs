namespace SupportChat.Server.Support.Application.ConversationAssignment;

// Owns conversation-scoped agent assignment: it reserves a capacity-available agent for
// a conversation and releases that reservation when the conversation closes. This keeps
// capacity accounting in the Application layer; the spot only orchestrates and notifies.
internal sealed class AgentAssignmentService(AgentAvailabilityDirectory availability)
{
    private readonly Dictionary<string, string> _reservations = new(StringComparer.Ordinal);

    // Availability is exposed through this service so reconnect can restore capacity
    // from the reservations that survived the temporary session disconnect.
    public void SetAvailable(string rosterActorId, string displayName, bool isAvailable)
    {
        var activeConversations = _reservations.Values.Count(id => string.Equals(id, rosterActorId, StringComparison.Ordinal));
        availability.SetAvailable(rosterActorId, displayName, isAvailable, activeConversations);
    }

    // Reserves a capacity-available agent for the conversation and returns it, or null
    // when the conversation is already assigned or no agent has spare capacity.
    public AvailableAgent? AssignForConversation(string conversationId)
    {
        if (_reservations.ContainsKey(conversationId)) return null;
        var assigned = availability.Assign();
        if (assigned is null) return null;

        _reservations[conversationId] = assigned.RosterActorId;
        return assigned;
    }

    // Frees the capacity a conversation reserved, if any (called on close).
    public void ReleaseConversation(string conversationId)
    {
        if (_reservations.Remove(conversationId, out var rosterActorId))
            availability.Release(rosterActorId);
    }
}
