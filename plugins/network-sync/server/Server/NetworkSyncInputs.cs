using Zlink.NetworkSync.Simulation;

namespace Zlink.NetworkSync.Server;

/// <summary>The inputs for one simulation step. The view is valid for that step.</summary>
public readonly ref struct NetworkSyncInputs<TInput, TAction>
{
    private readonly ReadOnlySpan<PlayerTickInput<TInput, TAction>> _inputs;

    internal NetworkSyncInputs(ReadOnlySpan<PlayerTickInput<TInput, TAction>> inputs) =>
        _inputs = inputs;

    public int Count => _inputs.Length;

    public NetworkSyncPlayerInput<TInput, TAction> this[int index] => new(_inputs[index]);
}

public readonly ref struct NetworkSyncPlayerInput<TInput, TAction>
{
    private readonly PlayerTickInput<TInput, TAction> _input;

    internal NetworkSyncPlayerInput(PlayerTickInput<TInput, TAction> input) => _input = input;

    public string ActorId => _input.ActorId;
    public TInput Input => _input.Input;
    public long? LastAppliedInputTick => _input.LastAppliedInputTick;
    public bool UsedFallbackInput => _input.UsedFallbackInput;
    public int ActionCount => _input.Actions.Length;

    public (ulong Id, TAction Payload) GetAction(int index)
    {
        var action = _input.Actions.Span[index];
        return (action.Id, action.Payload);
    }
}
