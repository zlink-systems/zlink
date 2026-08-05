namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkFrameworkHostLifecycleState
{
    private int _state = (int)ZLinkFrameworkRuntimeState.Preparing;

    internal ZLinkFrameworkRuntimeState State =>
        (ZLinkFrameworkRuntimeState)Volatile.Read(ref _state);

    internal event Action<ZLinkFrameworkRuntimeState>? Changed;

    internal void TransitionTo(ZLinkFrameworkRuntimeState state)
    {
        if (Interlocked.Exchange(ref _state, (int)state) == (int)state)
            return;
        Changed?.Invoke(state);
    }
}
