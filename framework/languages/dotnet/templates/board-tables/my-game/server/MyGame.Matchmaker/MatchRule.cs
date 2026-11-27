namespace MyGame.Matchmaker;

internal static class MatchRule
{
    // The queue is already ordered by priority and arrival time.
    public static Ticket[] Select(IEnumerable<Ticket> waiting) => waiting.Take(2).ToArray();
}
