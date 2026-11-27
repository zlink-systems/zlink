namespace Zlink.NetworkSync.Simulation;

internal readonly record struct SimulationAction<TAction>(ulong Id, TAction Payload);

internal readonly record struct PlayerTickInput<TInput, TAction>(
    string ActorId,
    TInput Input,
    ReadOnlyMemory<SimulationAction<TAction>> Actions,
    long? LastAppliedInputTick,
    bool UsedFallbackInput);
