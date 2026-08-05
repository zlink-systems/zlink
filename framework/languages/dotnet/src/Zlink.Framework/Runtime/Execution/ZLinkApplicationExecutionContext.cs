namespace Zlink.Framework.Runtime.Execution;

internal readonly record struct ZLinkApplicationExecutionScope(
    string SpotId,
    ZLinkUserSpotExecutionMode ExecutionMode,
    string? ActorId,
    bool YieldAllowed,
    Func<string, bool>? IsMemberActor = null);

internal static class ZLinkApplicationExecutionContext
{
    private static readonly AsyncLocal<ZLinkApplicationExecutionScope?> CurrentScope = new();

    public static ZLinkApplicationExecutionScope? Current => CurrentScope.Value;

    public static IDisposable Push(ZLinkApplicationExecutionScope scope)
    {
        var previous = CurrentScope.Value;
        CurrentScope.Value = scope;
        return new Revert(previous);
    }

    public static ZLinkSerialTurn RequireYieldTurn(string operation)
    {
        if (CurrentScope.Value is not { YieldAllowed: true }
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

    public static void RejectActorRequestWhenSameClaim(
        string targetActorId,
        ZLinkApplicationExecutionScope? capturedScope = null)
    {
        if ((CurrentScope.Value ?? capturedScope) is not { } current) return;
        if (current.ActorId is { } actorId
            && string.Equals(actorId, targetActorId, StringComparison.Ordinal))
            throw SameGate("An awaited request to the current Actor");
        if (current.ExecutionMode == ZLinkUserSpotExecutionMode.SpotWide
            && current.IsMemberActor?.Invoke(targetActorId) == true)
            throw SameGate("An awaited request to a member Actor of the current User Spot");
    }

    public static void RejectSpotRequestWhenSameGate(
        string targetSpotId,
        ZLinkApplicationExecutionScope? capturedScope = null)
    {
        if ((CurrentScope.Value ?? capturedScope) is
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
        if ((CurrentScope.Value ?? capturedScope) is not
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

    private static ZLinkFrameworkException SameGate(string operation)
    {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"{operation} would wait for the execution gate held by the current callback.");
    }

    private sealed class Revert(ZLinkApplicationExecutionScope? previous) : IDisposable
    {
        public void Dispose()
        {
            CurrentScope.Value = previous;
        }
    }
}
