namespace SupportChat.Server.Support.Application.ConversationAssignment;

// Tracks which agents are available and how many conversations each is currently
// handling. An agent stays available until it reaches capacity, so one agent can be
// assigned to several conversations at once (§9).
internal sealed class AgentAvailabilityDirectory(int capacity)
{
    private readonly Dictionary<string, AgentSlot> _agents = new(StringComparer.Ordinal);
    private readonly List<string> _order = [];

    public void SetAvailable(string rosterActorId, string displayName, bool isAvailable, int activeConversations)
    {
        if (!isAvailable)
        {
            _agents.Remove(rosterActorId);
            _order.Remove(rosterActorId);
            return;
        }

        if (!_agents.TryGetValue(rosterActorId, out var slot))
        {
            slot = new AgentSlot(rosterActorId);
            _agents[rosterActorId] = slot;
            _order.Add(rosterActorId);
        }

        slot.DisplayName = displayName;
        slot.Active = activeConversations;
    }

    // Picks an available agent that still has spare capacity and counts a new
    // conversation against it. Rotates the order for fairness. Returns null when no
    // agent has capacity.
    public AvailableAgent? Assign()
    {
        string? picked = null;
        foreach (var rosterId in _order)
        {
            if (_agents[rosterId].Active < capacity)
            {
                picked = rosterId;
                break;
            }
        }

        if (picked is null) return null;

        var slot = _agents[picked];
        slot.Active += 1;
        _order.Remove(picked);
        _order.Add(picked);
        return new AvailableAgent(slot.RosterActorId, slot.DisplayName);
    }

    // Frees one slot when a conversation the agent handled closes.
    public void Release(string rosterActorId)
    {
        if (_agents.TryGetValue(rosterActorId, out var slot) && slot.Active > 0) slot.Active -= 1;
    }

    private sealed class AgentSlot(string rosterActorId)
    {
        public string RosterActorId { get; } = rosterActorId;
        public string DisplayName { get; set; } = string.Empty;
        public int Active { get; set; }
    }
}

internal sealed record AvailableAgent(
    string RosterActorId,
    string DisplayName);
