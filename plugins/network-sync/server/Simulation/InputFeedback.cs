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
