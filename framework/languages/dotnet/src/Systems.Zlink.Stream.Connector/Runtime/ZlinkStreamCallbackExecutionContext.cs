namespace Systems.Zlink.Stream.Connector.Runtime;

/// <summary>
///     Records which connector's handler or callback the current flow is running, so that
///     <c>Close</c> and <c>DisposeAsync</c> can tell a call from inside one of them
///     (stream-connector spec §7).
/// </summary>
internal static class ZlinkStreamCallbackExecutionContext
{
    private static readonly AsyncLocal<Lease?> CallbackAmbient = new();

    public static bool IsActiveCallbackFor(object callbackOwner)
    {
        var lease = CallbackAmbient.Value;
        return lease is { Active: true } && ReferenceEquals(lease.CallbackOwner, callbackOwner);
    }

    public static IDisposable EnterCallback(object callbackOwner)
    {
        var current = new Lease(callbackOwner);
        var previous = CallbackAmbient.Value;
        CallbackAmbient.Value = current;
        return new Scope(previous, current);
    }

    private sealed class Lease(object callbackOwner)
    {
        public object CallbackOwner { get; } = callbackOwner;

        public bool Active { get; set; } = true;
    }

    private sealed class Scope(Lease? previous, Lease current) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
                return;
            current.Active = false;
            if (ReferenceEquals(CallbackAmbient.Value, current))
                CallbackAmbient.Value = previous;
        }
    }
}
