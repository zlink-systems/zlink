namespace Zlink.NetworkSync.Simulation.Tests;

internal sealed class FakeClock : TimeProvider
{
    private long _timestamp = 100_000;

    public override long TimestampFrequency => 1_000_000;

    public override long GetTimestamp() => _timestamp;

    public void Advance(long microseconds) => _timestamp += microseconds;
}

internal sealed class FakeSimulation(FakeClock clock) : INetworkSyncSimulation<int, int>
{
    public List<TickContext> Ticks { get; } = [];
    public List<PlayerTickInput<int, int>> Inputs { get; } = [];
    public int State { get; private set; }
    public int WorkMicroseconds { get; set; }

    public int GetMissingInput(string actorId) => 0;

    public void Step(TickContext tick, ReadOnlySpan<PlayerTickInput<int, int>> inputs)
    {
        Ticks.Add(tick);
        foreach (var input in inputs)
        {
            Inputs.Add(input);
            State += input.Input;
            foreach (var action in input.Actions.Span)
                State += action.Payload;
        }

        clock.Advance(WorkMicroseconds);
    }
}
