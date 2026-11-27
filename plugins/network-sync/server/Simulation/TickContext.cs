namespace Zlink.NetworkSync.Simulation;

internal readonly record struct TickContext(long Epoch, long Tick, long LogicalStartTimestamp);

internal readonly record struct WakeupResult(
    TickContext LatestTick,
    long BacklogTicks,
    int ExecutedTicks,
    long SkippedTicks,
    TimeSpan ExecutionTime,
    bool SnapshotDue);
