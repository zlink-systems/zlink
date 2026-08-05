namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotRelocationReadyCall(
    ZLinkSpotActivation activation) : IZLinkSpotRelocationReadyCall
{
    private int _submitted;

    public void Defer()
    {
        if (Interlocked.Exchange(ref _submitted, 1) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "RelocationReady Defer can be submitted only once.");

        ZLinkSpotRelocationReadyHandlerScope.Register(activation);
    }
}

internal sealed class ZLinkSpotRelocationReadyHandlerScope : IDisposable
{
    private static readonly AsyncLocal<ZLinkSpotRelocationReadyHandlerScope?>
        CurrentScope = new();

    private readonly ZLinkSpotActivation? _activation;
    private readonly ZLinkSpotRelocationReadyHandlerScope? _previous;
    private bool _completed;
    private bool _deferred;

    private ZLinkSpotRelocationReadyHandlerScope(
        ZLinkSpotActivation? activation)
    {
        _activation = activation;
        _previous = CurrentScope.Value;
        CurrentScope.Value = this;
    }

    internal static ZLinkSpotRelocationReadyHandlerScope Open(
        ZLinkSpotActivation? activation) =>
        new(activation);

    internal static void Register(ZLinkSpotActivation activation)
    {
        var scope = CurrentScope.Value
                    ?? throw Invalid(
                        "RelocationReady Defer is valid only in a Framework-managed Spot handler.");
        if (!ReferenceEquals(scope._activation, activation))
            throw Invalid(
                "RelocationReady Defer must target the Spot that owns the current handler.");
        if (activation.ExecutionMode != ZLinkUserSpotExecutionMode.SpotWide
            || activation.RelocationReadiness
            != ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
            throw Invalid(
                "RelocationReady Defer requires a SpotWide User Spot registered with ApplicationSignaled readiness.");
        if (scope._completed)
            throw Invalid(
                "RelocationReady Defer cannot register after the handler has completed.");
        if (scope._deferred)
            throw Invalid(
                "A handler turn can register RelocationReady only once.");

        scope._deferred = true;
    }

    internal static void EnsureOperationCanStart(
        ZLinkSpotActivation activation)
    {
        var scope = CurrentScope.Value;
        if (scope is null
            || !ReferenceEquals(scope._activation, activation)
            || !scope._deferred)
            return;

        throw Invalid(
            "A Spot handler cannot start another Framework operation after RelocationReady Defer.");
    }

    internal void Complete() => _completed = true;

    public void Dispose()
    {
        CurrentScope.Value = _previous;
        if (_completed && _deferred)
            _activation!.CompleteRelocationReadyTurn();
    }

    private static ZLinkFrameworkException Invalid(string message) =>
        new(ZLinkFrameworkErrorKind.InvalidOperation, message);
}
