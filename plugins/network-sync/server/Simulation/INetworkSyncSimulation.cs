namespace Zlink.NetworkSync.Simulation;

internal interface INetworkSyncSimulation<TInput, TAction>
{
    /// <summary>Supplies the neutral input when there is no usable hold input.</summary>
    TInput GetMissingInput(string actorId);

    /// <summary>
    /// Applies one tick to the authoritative game state. Inputs can be read for the duration of the call.
    /// A callback failure propagates to the match owner, which must retire a partially applied game state.
    /// </summary>
    void Step(TickContext tick, ReadOnlySpan<PlayerTickInput<TInput, TAction>> inputs);
}
