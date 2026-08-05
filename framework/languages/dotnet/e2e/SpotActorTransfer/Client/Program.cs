using SpotActorTransfer.Client.Scenarios;
using SpotActorTransfer.Client.Support;

var options = ClientOptions.Parse(args);
using var context = new SpotActorTransferScenarioContext(options);
await context.WaitMeshReadyAsync();

var scenarios = new Dictionary<string, Func<Task>>(StringComparer.OrdinalIgnoreCase)
{
    ["ST-A1"] = () => StA1LocalAcceptScenario.RunAsync(context),
    ["ST-A2"] = () => StA2LocalRejectScenario.RunAsync(context),
    ["ST-A3"] = () => StA3MovingDispatchBlockedScenario.RunAsync(context),
    ["ST-B1"] = () => StB1RemoteStatefulTransferScenario.RunAsync(context),
    ["ST-B2"] = () => StB2SourceCleanupFailureAfterSuccessScenario.RunAsync(context),
    ["ST-B3"] = () => StB3MissingAdapterScenario.RunAsync(context),
    ["ST-B4"] = () => StB4EmptyStateTransferScenario.RunAsync(context),
    ["ST-B5"] = () =>
        StB5TargetCrashAggregateUnavailableScenario.RunAsync(context),
    ["ST-C1"] = () => StC1SourceDownBeforeCommitScenario.RunAsync(context),
    ["ST-C2"] = () => StC2SourceDownAfterTargetCommitScenario.RunAsync(context),
    ["ST-C3"] = () => StC3CallbackFailureClassificationScenario.RunAsync(context),
    ["ST-D1"] = () => StD1LocationCommitTimingScenario.RunAsync(context),
    ["ST-D2"] = () => StD2StaleSourceReleaseFencingScenario.RunAsync(context),
    ["ST-E1"] = () => StE1BoundSessionPushAfterTransferScenario.RunAsync(context),
    ["ST-E1A"] = () => StE1ANewIncarnationExplicitBindScenario.RunAsync(context),
    ["ST-E2"] = () => StE2BoundSessionRebindIsolationScenario.RunAsync(context),
    ["ST-F1"] = () => StF1InFlightHandoffOrderScenario.RunAsync(context),
    ["ST-F2"] = () => StF2DirectOvertakePreventionScenario.RunAsync(context),
    ["ST-F3"] = () => StF3BoundSessionCrossMoveOrderScenario.RunAsync(context),
    ["ST-F4"] = () => StF4MessageFollowThenRejectScenario.RunAsync(context),
    ["ST-F5"] = () => StF5MessageFollowRouteRemovalScenario.RunAsync(context),
    ["ST-F6"] = () => StF6InFlightRequestCorrelationAndTimeoutScenario.RunAsync(context),
    ["ST-G3"] = () => StG3PerActorShellRelocationScenario.RunAsync(context),
    ["ST-G5-SMALL"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSmallAsync(context),
    ["ST-G5-ENTRY-ACTOR-SMALL"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSmallAsync(context),
    ["ST-G5-SLOW-CAPTURE"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowCaptureAsync(context),
    ["ST-G5-ENTRY-ACTOR-SLOW-CAPTURE"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowCaptureAsync(context),
    ["ST-G5-SLOW-RESTORE"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowRestoreAsync(context),
    ["ST-G5-ENTRY-ACTOR-SLOW-RESTORE"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowRestoreAsync(context),
    ["ST-G5-SLOW-CLEANUP"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowCleanupAsync(context),
    ["ST-G5-ENTRY-ACTOR-SLOW-CLEANUP"] = () =>
        StG5ActorRelocationInterruptionScenario.RunSlowCleanupAsync(context),
    ["ST-G5-PER-ACTOR-SMALL"] = () =>
        StG5NonAggregateRelocationInterruptionScenario.RunPerActorAsync(
            context),
    ["ST-G5-PER-ACTOR-SLOW-CAPTURE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunPerActorSlowCaptureAsync(context),
    ["ST-G5-PER-ACTOR-SLOW-RESTORE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunPerActorSlowRestoreAsync(context),
    ["ST-G5-PER-ACTOR-SPOT-SMALL"] = () =>
        StG5NonAggregateRelocationInterruptionScenario.RunPerActorSpotAsync(
            context),
    ["ST-G5-PER-ACTOR-SPOT-SLOW-CAPTURE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunPerActorSpotSlowCaptureAsync(context),
    ["ST-G5-PER-ACTOR-SPOT-SLOW-RESTORE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunPerActorSpotSlowRestoreAsync(context),
    ["ST-G5-INSTANCE-SPOT-SMALL"] = () =>
        StG5NonAggregateRelocationInterruptionScenario.RunInstanceSpotAsync(
            context),
    ["ST-G5-INSTANCE-SPOT-SLOW-CAPTURE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunInstanceSpotSlowCaptureAsync(context),
    ["ST-G5-INSTANCE-SPOT-SLOW-RESTORE"] = () =>
        StG5NonAggregateRelocationInterruptionScenario
            .RunInstanceSpotSlowRestoreAsync(context),
    ["ST-G5-SPOT-WIDE-ACTORS-10"] = () =>
        StG5SpotWideRelocationInterruptionScenario.RunActors10Async(context),
    ["ST-G5-SPOT-WIDE-ACTORS-100"] = () =>
        StG5SpotWideRelocationInterruptionScenario.RunActors100Async(context),
    ["ST-G6"] = () =>
        StG6ApplicationSignaledRelocationScenario.RunAsync(context),
    ["ST-I1"] = () => StI1RelocationPayloadMeasurementScenario.RunAsync(context),
    ["ST-I1-ACTOR-BOUNDARY"] = () =>
        StI1RelocationPayloadMeasurementScenario
            .RunActorBoundaryRelocationOnlyAsync(context),
    ["ST-I1-INSTANCE"] = () =>
        StI1RelocationPayloadMeasurementScenario
            .RunInstanceRelocationOnlyAsync(context),
    ["ST-I1-SPOTWIDE"] = () =>
        StI1RelocationPayloadMeasurementScenario
            .RunSpotWideRelocationOnlyAsync(context),
    ["ST-I1-SPOTWIDE-SMALL"] = () =>
        StI1RelocationPayloadMeasurementScenario
            .RunSpotWideSmallRelocationOnlyAsync(context),
    ["ST-I1-SPOTWIDE-BOUNDARY"] = () =>
        StI1RelocationPayloadMeasurementScenario
            .RunSpotWideBoundaryRelocationOnlyAsync(context),
    ["ST-I2-RECREATE-ON-RELOCATION"] = () =>
        StI2BulkActorRelocationScenario.RunRecreateOnRelocationAsync(context),
    ["ST-I2-PRESERVE-STATE-WITH"] = () =>
        StI2BulkActorRelocationScenario.RunPreserveStateWithAsync(context),
    // Compatibility aliases for earlier Config 10 runner names.
    ["ST-I2-RECREATE"] = () =>
        StI2BulkActorRelocationScenario.RunRecreateOnRelocationAsync(context),
    ["ST-I2-SNAPSHOT"] = () =>
        StI2BulkActorRelocationScenario.RunPreserveStateWithAsync(context),
    ["ST-I3-INSTANCE"] = () =>
        StI3BulkSpotRelocationScenario.RunInstanceAsync(context),
    ["ST-I3-SPOTWIDE"] = () =>
        StI3BulkSpotRelocationScenario.RunSpotWideAsync(context),
    ["ST-I4"] = () => StI4ActorMessageFollowMatrixScenario.RunAsync(context),
    ["MF-AO-FOLLOW"] = () =>
        StI4ActorMessageFollowMatrixScenario.RunAsync(context),
    ["MF-AR-FOLLOW"] = () =>
        StI4ActorMessageFollowMatrixScenario.RunAsync(context),
    ["MF-AO-QUEUE"] = () =>
        StI4RelocationAuthorityBoundaryScenario.RunActorQueueAsync(context),
    ["MF-AR-HOLD"] = () =>
        StI4RelocationAuthorityBoundaryScenario.RunActorHoldAsync(context),
    ["MF-SO-QUEUE"] = () =>
        StI4RelocationAuthorityBoundaryScenario.RunSpotQueueAsync(context),
    ["MF-SR-HOLD"] = () =>
        StI4RelocationAuthorityBoundaryScenario.RunSpotHoldAsync(context),
    ["MF-SO-FOLLOW"] = () =>
        StI4SpotMessageFollowMatrixScenario.RunAsync(context),
    ["MF-SR-FOLLOW"] = () =>
        StI4SpotMessageFollowMatrixScenario.RunAsync(context),
    ["ST-I5"] = () => StI5MessageFollowSafetyScenario.RunAsync(context),
    ["MF-CORR"] = () => StI5MessageFollowSafetyScenario.RunAsync(context),
    ["ST-I6"] = () => StI6ActorMultiHopMessageFollowScenario.RunAsync(context),
    ["ST-H1"] = () => StH1DeferredJoinBarrierScenario.RunAsync(context)
};

var knownContractGaps = new Dictionary<string, string>(
    StringComparer.OrdinalIgnoreCase)
{
    ["MF-PA-SPLIT"] =
        "PerActor Spot and Actor split routing is not implemented.",
    ["MF-DUP"] =
        "Duplicate stale delivery is not implemented.",
    ["MF-EXP"] =
        "Route-absent, duration-zero, and expired cases are not all implemented.",
    ["MF-GEN"] =
        "Previous ObjectGeneration delivery is not implemented.",
    ["MF-LOOP"] =
        "Message Follow loop rejection is not implemented.",
    ["MF-HOP"] =
        "Eight-hop acceptance and ninth-hop rejection are not implemented.",
    ["MF-BOUND"] =
        "The 1,024-record and 16 MiB Message Follow bounds are not implemented."
};

var excludedFromAll = new HashSet<string>(
    [
        "ST-F4",
        "ST-F5",
        "ST-G3",
        "ST-G5-SMALL",
        "ST-G5-ENTRY-ACTOR-SMALL",
        "ST-G5-SLOW-CAPTURE",
        "ST-G5-ENTRY-ACTOR-SLOW-CAPTURE",
        "ST-G5-SLOW-RESTORE",
        "ST-G5-ENTRY-ACTOR-SLOW-RESTORE",
        "ST-G5-SLOW-CLEANUP",
        "ST-G5-ENTRY-ACTOR-SLOW-CLEANUP",
        "ST-G5-PER-ACTOR-SMALL",
        "ST-G5-PER-ACTOR-SLOW-CAPTURE",
        "ST-G5-PER-ACTOR-SLOW-RESTORE",
        "ST-G5-PER-ACTOR-SPOT-SMALL",
        "ST-G5-PER-ACTOR-SPOT-SLOW-CAPTURE",
        "ST-G5-PER-ACTOR-SPOT-SLOW-RESTORE",
        "ST-G5-INSTANCE-SPOT-SMALL",
        "ST-G5-INSTANCE-SPOT-SLOW-CAPTURE",
        "ST-G5-INSTANCE-SPOT-SLOW-RESTORE",
        "ST-G5-SPOT-WIDE-ACTORS-10",
        "ST-G5-SPOT-WIDE-ACTORS-100",
        "ST-G6",
        "ST-I1",
        "ST-I1-ACTOR-BOUNDARY",
        "ST-I1-INSTANCE",
        "ST-I1-SPOTWIDE",
        "ST-I1-SPOTWIDE-SMALL",
        "ST-I1-SPOTWIDE-BOUNDARY",
        "ST-I2-RECREATE",
        "ST-I2-SNAPSHOT",
        "ST-I2-RECREATE-ON-RELOCATION",
        "ST-I2-PRESERVE-STATE-WITH",
        "ST-I3-INSTANCE",
        "ST-I3-SPOTWIDE",
        "ST-I4",
        "MF-AO-FOLLOW",
        "MF-AR-FOLLOW",
        "MF-AO-QUEUE",
        "MF-AR-HOLD",
        "MF-SO-QUEUE",
        "MF-SR-HOLD",
        "MF-SO-FOLLOW",
        "MF-SR-FOLLOW",
        "ST-I5",
        "MF-CORR",
        "ST-I6",
        "ST-H1"
    ],
    StringComparer.OrdinalIgnoreCase);
var selected = (string.Equals(options.Scenario, "all", StringComparison.OrdinalIgnoreCase)
    ? scenarios.Keys.Where(name => !excludedFromAll.Contains(name))
    : options.Scenario.Split(
        ',',
        StringSplitOptions.RemoveEmptyEntries
        | StringSplitOptions.TrimEntries))
    .ToArray();
if (selected.Any(static name =>
        name.Equals("ST-I2-RECREATE", StringComparison.OrdinalIgnoreCase)
        || name.Equals(
            "ST-I2-SNAPSHOT",
            StringComparison.OrdinalIgnoreCase)
        || name.Equals(
            "ST-I2-RECREATE-ON-RELOCATION",
            StringComparison.OrdinalIgnoreCase)
        || name.Equals(
            "ST-I2-PRESERVE-STATE-WITH",
            StringComparison.OrdinalIgnoreCase))
    && selected.Length != 1)
{
    throw new ArgumentException(
        "Each ST-I2 profile requires its own fresh host-process run.");
}
if (selected.Any(static name => name.StartsWith(
        "ST-G5-",
        StringComparison.OrdinalIgnoreCase))
    && selected.Length != 1)
{
    throw new ArgumentException(
        "Each ST-G5 service-unit profile requires its own fresh host-process run.");
}
if (selected.Any(static name => name.ToUpperInvariant() is
        "MF-AO-QUEUE"
        or "MF-AR-HOLD"
        or "MF-SO-QUEUE"
        or "MF-SR-HOLD"
        or "MF-SO-FOLLOW"
        or "MF-SR-FOLLOW")
    && selected.Length != 1)
{
    throw new ArgumentException(
        "Each Message Follow authority-boundary selector requires its own "
        + "fresh host-process run.");
}
var diagnosticOnlyRun = false;
foreach (var name in selected)
{
    if (!scenarios.TryGetValue(name, out var scenario))
    {
        if (knownContractGaps.TryGetValue(name, out var gap))
            throw new NotSupportedException(
                $"Scenario '{name}' is a public contract gap: {gap}");
        throw new ArgumentException($"Unknown scenario '{name}'.");
    }
    await scenario();
    var diagnosticOnly = IsDiagnosticOnly(name);
    diagnosticOnlyRun |= diagnosticOnly;
    Console.WriteLine(
        diagnosticOnly
            ? $"operation SpotActorTransfer.{name} diagnostic_only"
            : $"operation SpotActorTransfer.{name} passed");
}

Console.WriteLine(
    diagnosticOnlyRun
        ? "spot-actor-transfer e2e result=diagnostic_only"
        : "spot-actor-transfer e2e result=passed");

static bool IsDiagnosticOnly(string name)
{
    return name.ToUpperInvariant() switch
    {
        // ST-I1 currently covers payload profiles and Store read-back only.
        // The selector must not report full completion until queue, journal,
        // timer, permit-contention, and aggregate-bound cases are implemented.
        "ST-I1" => true,
        // These selectors remain diagnostic until interruption and resource
        // measurements are recorded without private runtime hooks.
        "ST-I2-RECREATE" or "ST-I2-SNAPSHOT"
            or "ST-I2-RECREATE-ON-RELOCATION"
            or "ST-I2-PRESERVE-STATE-WITH"
            or "ST-I3-INSTANCE" => true,
        // Final owner equality is not a SpotWide pre/post visibility proof.
        "ST-I3-SPOTWIDE" => true,
        // ACTORS-100 owns the canonical one-second scale gate and fails in the
        // scenario when that bound is exceeded. It remains diagnostic only
        // because atomic publication and stale-route Spot Message Follow are
        // not yet observable through public contracts. ACTORS-10 is the fast
        // regression profile.
        "ST-G5-SPOT-WIDE-ACTORS-100" => true,
        _ => false
    };
}
