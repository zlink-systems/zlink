using Zlink.Framework.UnitTests;
using Zlink.Framework.UnitTests.Runtime;

// Run the existing assertions in an isolated process: limiting VSTest itself
// to one worker can starve its test-host startup and communication tasks.
// Actor Join uses DOTNET_PROCESSOR_COUNT=1 plus CPU affinity; the earlier
// regressions additionally require a hard one-worker ThreadPool limit.
var actorJoinOnly = args.Contains("--actor-join");
ThreadPool.GetMinThreads(out _, out var minIoThreads);
ThreadPool.GetMaxThreads(out _, out var maxIoThreads);
if (!actorJoinOnly && (!ThreadPool.SetMinThreads(1, minIoThreads)
    || !ThreadPool.SetMaxThreads(1, maxIoThreads)))
    throw new InvalidOperationException("Could not limit the ThreadPool to one worker.");
ThreadPool.GetMaxThreads(out var maxWorkers, out _);
Console.WriteLine($"ThreadPool maximum workers: {maxWorkers}");

var timer = new TimerLifecycleTests();
var maintenance = new MaintenanceRuntimeTests();
var spot = new EntrySpotActorDispatchTests();
var relocation = new RelocationBehaviorConformanceTests();
(string Name, Func<Task> Run)[] regressions =
[
    (nameof(timer.Concurrent_cancel_callers_observe_the_same_cleanup_failure_after_pump_completion),
        timer.Concurrent_cancel_callers_observe_the_same_cleanup_failure_after_pump_completion),
    (nameof(maintenance.Shutdown_expired_before_drain_still_invokes_force_stop),
        maintenance.Shutdown_expired_before_drain_still_invokes_force_stop),
    (nameof(spot.Current_Spot_Publish_Emits_Sent_With_Spot_Rid_And_Current_Flow),
        spot.Current_Spot_Publish_Emits_Sent_With_Spot_Rid_And_Current_Flow),
    (nameof(relocation.ActorJoin_runtime_preserves_observable_order_and_exact_callback_counts),
        relocation.ActorJoin_runtime_preserves_observable_order_and_exact_callback_counts),
    (nameof(relocation.ActorJoin_repeated_moves_release_source_instances_and_membership_obligations),
        relocation.ActorJoin_repeated_moves_release_source_instances_and_membership_obligations)
];

var failures = 0;
foreach (var regression in regressions)
{
    if (regression.Name.StartsWith("ActorJoin_", StringComparison.Ordinal) != actorJoinOnly)
        continue;
    try
    {
        await regression.Run();
        Console.WriteLine($"PASS {regression.Name}");
    }
    catch (Exception exception)
    {
        failures++;
        Console.Error.WriteLine($"FAIL {regression.Name}: {exception}");
    }
}
return failures == 0 ? 0 : 1;
