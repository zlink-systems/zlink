namespace Systems.Zlink.Stream.Connector.Runtime;

internal static class ZlinkStreamCallbackExecutionContext
{
    private static readonly AsyncLocal<Lease?> CallbackAmbient = new();
    private static readonly AsyncLocal<Lease?> WorkerAmbient = new();

    public static bool IsActiveCallbackFor(object callbackOwner)
    {
        var lease = CallbackAmbient.Value;
        return lease is { Active: true } && ReferenceEquals(lease.CallbackOwner, callbackOwner);
    }

    public static ZlinkStreamLifecycleWorkKind? CurrentWorkerCallbackKindFor(object workerOwner)
    {
        var lease = CallbackAmbient.Value;
        return lease is { Active: true } && ReferenceEquals(lease.WorkerOwner, workerOwner)
            ? lease.WorkKind
            : null;
    }

    public static IDisposable EnterWorker(object owner, ZlinkStreamLifecycleWorkKind workKind)
    {
        return Enter(WorkerAmbient, Lease.ForWorker(owner, workKind));
    }

    public static IDisposable EnterCallback(object callbackOwner)
    {
        var worker = WorkerAmbient.Value;
        var current = worker is { Active: true }
            ? Lease.ForCallback(callbackOwner, worker.WorkerOwner, worker.WorkKind)
            : Lease.ForCallback(callbackOwner, null, null);
        return Enter(CallbackAmbient, current);
    }

    private static IDisposable Enter(AsyncLocal<Lease?> ambient, Lease current)
    {
        var previous = ambient.Value;
        ambient.Value = current;
        return new Scope(ambient, previous, current);
    }

    private sealed class Lease(
        object? callbackOwner,
        object? workerOwner,
        ZlinkStreamLifecycleWorkKind? workKind)
    {
        public object? CallbackOwner { get; } = callbackOwner;

        public object? WorkerOwner { get; } = workerOwner;

        public ZlinkStreamLifecycleWorkKind? WorkKind { get; } = workKind;

        public bool Active { get; set; } = true;

        public static Lease ForCallback(
            object callbackOwner,
            object? workerOwner,
            ZlinkStreamLifecycleWorkKind? workKind) =>
            new(callbackOwner, workerOwner, workKind);

        public static Lease ForWorker(object workerOwner, ZlinkStreamLifecycleWorkKind workKind) =>
            new(null, workerOwner, workKind);
    }

    private sealed class Scope(
        AsyncLocal<Lease?> ambient,
        Lease? previous,
        Lease current) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            current.Active = false;
            if (ReferenceEquals(ambient.Value, current)) ambient.Value = previous;
        }
    }
}

internal enum ZlinkStreamLifecycleWorkKind
{
    ActiveConnect,
    Receive,
    Heartbeat,
    CloseCompletion
}
