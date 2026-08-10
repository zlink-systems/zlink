namespace Zlink.Framework.Runtime.Execution;

internal readonly record struct ZLinkApplicationExecutionScope(
    string SpotId,
    ZLinkUserSpotExecutionMode ExecutionMode,
    string? ActorId,
    bool YieldAllowed,
    Func<string, bool>? IsMemberActor = null,
    ZLinkApplicationExecutionClaim? Claim = null);

internal sealed class ZLinkApplicationExecutionClaim
{
    private int _active = 1;

    public bool IsActive => Volatile.Read(ref _active) != 0;

    public void Deactivate() => Interlocked.Exchange(ref _active, 0);
}

internal enum ZLinkNestedRequestTerminator
{
    Async = 0,
    Yield = 1
}

internal static class ZLinkApplicationExecutionContext
{
    private static readonly AsyncLocal<ZLinkApplicationExecutionScope?> CurrentScope = new();

    public static ZLinkApplicationExecutionScope? Current =>
        CurrentScope.Value is { Claim.IsActive: true } current
            ? current
            : null;

    public static IDisposable Push(ZLinkApplicationExecutionScope scope)
    {
        var previous = CurrentScope.Value;
        var claim = new ZLinkApplicationExecutionClaim();
        CurrentScope.Value = scope with { Claim = claim };
        return new Revert(previous, claim);
    }

    public static ZLinkSerialTurn RequireYieldTurn(string operation)
    {
        if (Current is not { YieldAllowed: true }
            || ZLinkSerialTurn.Current is not { } turn)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"{operation} Yield is only valid in a SpotWide User Spot or Instance Spot application callback.");

        return turn;
    }

    public static ZLinkSerialTurn RequireYieldTurn(
        ZLinkSerialTurn? capturedTurn,
        string operation)
    {
        var current = RequireYieldTurn(operation);
        if (!ReferenceEquals(current, capturedTurn))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"{operation} Yield must execute in the callback turn that created the call.");
        return current;
    }

    public static void ValidateActorRequest(
        string targetActorId,
        ZLinkNestedRequestTerminator terminator,
        ZLinkApplicationExecutionScope? capturedScope = null)
    {
        if (!TryResolveActive(capturedScope, out var current)) return;
        if (current.ActorId is { } actorId
            && string.Equals(actorId, targetActorId, StringComparison.Ordinal))
            throw SameGate("An awaited request to the current Actor");
        if (terminator == ZLinkNestedRequestTerminator.Async
            && current.ExecutionMode == ZLinkUserSpotExecutionMode.SpotWide
            && current.IsMemberActor?.Invoke(targetActorId) == true)
            throw SameGate("An awaited request to a member Actor of the current User Spot");
    }

    public static void ValidateSpotRequest(
        string targetSpotId,
        ZLinkNestedRequestTerminator terminator,
        ZLinkApplicationExecutionScope? capturedScope = null)
    {
        if (terminator == ZLinkNestedRequestTerminator.Async
            && TryResolveActive(capturedScope, out var current)
            && current is
            {
                ExecutionMode: ZLinkUserSpotExecutionMode.SpotWide,
                SpotId: var spotId
            }
            && string.Equals(spotId, targetSpotId, StringComparison.Ordinal))
            throw SameGate("An awaited request to the current User Spot");
    }

    public static void RejectActorJoinWhenSameGate(
        string? targetSpotId,
        ZLinkApplicationExecutionScope? capturedScope = null)
    {
        if (!TryResolveActive(capturedScope, out var current)
            || current is not
            {
                ExecutionMode: ZLinkUserSpotExecutionMode.SpotWide,
                ActorId: not null,
                SpotId: var currentSpotId
            })
            return;

        if (targetSpotId is null
            || !string.Equals(currentSpotId, targetSpotId, StringComparison.Ordinal))
            throw SameGate("Actor join from a SpotWide User Spot callback");
    }

    private static bool TryResolveActive(
        ZLinkApplicationExecutionScope? capturedScope,
        out ZLinkApplicationExecutionScope scope)
    {
        if (CurrentScope.Value is { Claim.IsActive: true } current)
        {
            scope = current;
            return true;
        }
        if (capturedScope is { Claim.IsActive: true } captured)
        {
            scope = captured;
            return true;
        }

        scope = default;
        return false;
    }

    private static ZLinkFrameworkException SameGate(string operation)
    {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"{operation} would wait for the execution gate held by the current callback.");
    }

    private sealed class Revert(
        ZLinkApplicationExecutionScope? previous,
        ZLinkApplicationExecutionClaim claim) : IDisposable
    {
        public void Dispose()
        {
            claim.Deactivate();
            CurrentScope.Value = previous;
        }
    }
}
