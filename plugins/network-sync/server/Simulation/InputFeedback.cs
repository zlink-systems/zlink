namespace Zlink.NetworkSync.Simulation;

internal readonly record struct InputFeedback(InputDisposition Disposition, TimeSpan? ArrivalMargin = null);

internal enum InputDisposition
{
    Accepted,
    WrongMatch,
    WrongEpoch,
    UnknownActor,
    InvalidSize,
    RateLimited,
    InvalidTick,
    TooFarAhead,
    Late,
    DuplicateTick,
    DuplicateAction
}

/// <summary>Wire v1 <c>InputDisposition</c> (plugins/network-sync/protocol/wire-v1.json).</summary>
internal enum WireInputDisposition
{
    Accepted,
    Late,
    Duplicate,
    Rejected
}

internal static class InputDispositionWire
{
    /// <summary>The single mapping from acceptance reasons to the wire v1 disposition.</summary>
    public static WireInputDisposition ToWire(this InputDisposition disposition) => disposition switch
    {
        InputDisposition.Accepted => WireInputDisposition.Accepted,
        InputDisposition.Late => WireInputDisposition.Late,
        InputDisposition.DuplicateTick or InputDisposition.DuplicateAction => WireInputDisposition.Duplicate,
        InputDisposition.WrongMatch or InputDisposition.WrongEpoch or InputDisposition.UnknownActor
            or InputDisposition.InvalidSize or InputDisposition.RateLimited or InputDisposition.InvalidTick
            or InputDisposition.TooFarAhead => WireInputDisposition.Rejected,
        _ => throw new ArgumentOutOfRangeException(nameof(disposition), disposition, null)
    };
}
