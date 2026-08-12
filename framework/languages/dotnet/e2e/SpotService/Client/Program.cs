using System.Text.Json;
using SpotService.Client.Scenarios;
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);

using var gateway = ZLinkHttpClient.Create(options.GatewayUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();
using var playA = ZLinkHttpClient.Create(options.PlayAUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();
using var playB = ZLinkHttpClient.Create(options.PlayBUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();
using var multiA = ZLinkHttpClient.Create(options.MultiAUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();
using var multiB = ZLinkHttpClient.Create(options.MultiBUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();
using var sessionA = ZLinkHttpClient.Create(options.SessionAUrl)
    .Timeout(TimeSpan.FromMinutes(5))
    .Build();

await (options.OperationGroup switch
{
    "sm-a1" => SmA1EntrySpotRequestScenario.RunAsync(playA),
    "sm-a2" => SmA2UserSpotStateMutationScenario.RunAsync(playA),
    "sm-a3" => SmA3SpecificSpotOwnerRoutingScenario.RunAsync(playA, playB),
    "sm-a4" => SmA4OwnerRoutingScenario.RunAsync(playA),
    "sm-a5" => SmA5ScenarioStageLifecycleScenario.RunAsync(playA),
    "sm-a6" => SmA6SpotInitializeCloseLifecycleScenario.RunAsync(playA),
    "sm-a7" => SmA7SpotTypeMismatchScenario.RunAsync(playA),
    "sm-a8" => SmA8WorkerOffloadScenario.RunAsync(playA),
    "sm-a9" => SmA9SpotPublicationBarrierScenario.RunAsync(playA, playB),
    "sm-a10" => SmA10EntrySpotIdentityScenario.RunAsync(playA),
    "sm-a11" => SmA11ReservedEntrySpotIdScenario.RunAsync(playA),
    "sm-a12" => SmA12AutomaticSpotIdsScenario.RunAsync(playA),
    "sm-a13" => SmA13SpotIdBoundaryScenario.RunAsync(playA),
    "sm-b1" => SmB1LocalActorJoinScenario.RunAsync(playA, sessionA, options.SessionAStreamEndpoint),
    "sm-b0" => SmB0ActorManagerLifecycleScenario.RunAsync(gateway, playA, playB),
    "sm-b0a" => SmB0AActorCreationRaceScenario.RunAsync(gateway, playA, playB),
    "sm-b2" => SmB2RemoteActorJoinScenario.RunAsync(playB, sessionA, options.SessionAStreamEndpoint),
    "sm-b3" => SmB3RequestMessageFidelityScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-b4" => SmB4RemoteActorRequestReplyScenario.RunAsync(playB, options.SessionAStreamEndpoint),
    "sm-b6" => SmB6ActorDisconnectCallbackScenario.RunAsync(playA, playB, sessionA, options.SessionAStreamEndpoint),
    "sm-b7" => SmB7ActorLifecycleOrderScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-b8" => SmB8ExplicitActorDestroyScenario.RunAsync(gateway),
    "sm-b9" => SmB9ActorJoinAdmissionScenario.RunAsync(playA, playB, options.SessionAStreamEndpoint),
    "sm-b11" => SmB11ActorPublicationBarrierScenario.RunAsync(gateway, playA, playB),
    "sm-b10" => SmB10ObjectRolePrerequisiteScenario.RunAsync(
        playA,
        options.B10ControlEndpoint,
        options.B10ControlRid),
    "sm-c1" => RunC1Async(playA, playB),
    "sm-c2" => RunC2Async(playA, playB),
    "sm-c3" => SmC3SpotMeshMessagingScenario.RunAsync(playA, playB),
    "sm-c4" => SmC4SpotPublisherClientScenario.RunAsync(playA, gateway),
    "sm-c5" => SmC5RemoteSpotPublishSubscribeScenario.RunAsync(playA, playB),
    "sm-c6" => SmC6LogicalMulticastBackpressureScenario.RunAsync(
        gateway,
        playA,
        playB,
        options.SmC6PauseAckFile,
        options.SmC6ResumeAckFile),
    "sm-d2" => RunD2Async(playA, playB, sessionA, options.SessionAStreamEndpoint),
    "sm-d3" => SmD3EntryAndUserSpotSessionRelayScenario.RunAsync(
        playA,
        playB,
        options.SessionAStreamEndpoint),
    "sm-d4" => SmD4MultipleActorBindingScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d4a" => SmD4AStaleBindingIsolationScenario.RunAsync(
        playA,
        gateway,
        options.SessionAStreamEndpoint,
        options.SessionBStreamEndpoint),
    "sm-d4b" => SmD4BStoredRouteWithoutStoreScenario.RunAsync(
        gateway,
        playA,
        playB,
        sessionA,
        options.SessionAStreamEndpoint,
        options.SessionBStreamEndpoint,
        [
            options.PlayATransportProxyAdmin,
            options.PlayBTransportProxyAdmin,
            options.SessionATransportProxyAdmin
        ]),
    "sm-d5" => SmD5ExplicitDisconnectNotificationScenario.RunAsync(
        gateway,
        playA,
        playB,
        sessionA,
        options.SessionAStreamEndpoint),
    "sm-d5a" => SmD5ALogicalDisconnectScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-d6" => SmD6BoundSessionPushTargetingScenario.RunAsync(options.SessionAStreamEndpoint, options.SessionBStreamEndpoint),
    "sm-d7" => SmD7StreamAuthenticationDispatchScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d8" => SmD8StreamReconnectRecoveryScenario.RunAsync(
        playA,
        playB,
        options.SessionAStreamEndpoint),
    "sm-d9" => SmD9StreamInboundObserverScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d10" => SmD10BoundedSessionBackpressureScenario.RunAsync(options.SessionAStreamEndpoint, options.SessionBStreamEndpoint),
    "sm-d11" => SmD11StreamAndRouteRequestScenario.RunAsync(sessionA, options.SessionAStreamEndpoint),
    "sm-d12" => SmD12SessionReconnectMigrationScenario.RunAsync(
        playA,
        playB,
        options.SessionAStreamEndpoint,
        options.SessionBStreamEndpoint),
    "sm-d13" => SmD13HeartbeatRequestScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d14" => SmD14TlsStreamValidationScenario.RunAsync(options.SessionATlsStreamEndpoint),
    "sm-d15" => SmD15GatewayActorSessionPushScenario.RunAsync(playA, gateway, options.SessionAStreamEndpoint),
    "sm-e1" => SmE1MissingSpotRouteScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-e2" => SmE2SpotTimerTickScenario.RunAsync(playA),
    "sm-e3" => SmE3IdleTimerSpotCloseScenario.RunAsync(playA),
    "sm-e4" => RunE4Async(playA, playB),
    "sm-f1" => SmF1ClientServerChannelToSpotScenario.RunAsync(playA),
    "sm-f2" => SmF2RouteMeshChannelToSpotScenario.RunAsync(playA, playB),
    "sm-f3" => RunF3Async(playA, playB, gateway),
    "sm-f4" => SmF4MissingTargetSpotRouteScenario.RunAsync(playA),
    "sm-f5" => SmF5ClosedSpotRouteIsolationScenario.RunAsync(playA, gateway),
    "sm-f6" => SmF6RemoteSpotOutboundScenario.RunAsync(multiA, multiB),
    "sm-g1" => SmG1BoundActorCrashRecoveryScenario.RunAsync(
        playA, playB, gateway, options.SessionAStreamEndpoint, options.SessionBStreamEndpoint),
    "sm-g2" => SmG2OwnerSpotRemapScenario.RunAsync(playA, playB, gateway),
    "sm-g3" => SmG3ConcurrentSessionActorLifecycleScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-g4" => SmG4ConcurrentBoundSessionPushScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-g5a" => SmG5AAndG5BPlacementScenario.RunWeightDistributionAsync(gateway, playA, playB),
    "sm-g5b" => SmG5AAndG5BPlacementScenario.RunCapacityEligibilityAsync(gateway, playA, playB),
    "sm-g5" => RunG5Async(gateway, playA, playB),
    "sm-b1-b2-b3" => RunB1B2B3Async(playA, playB, sessionA, options.SessionAStreamEndpoint),
    "sm-d2-d6" => RunD2D6Async(
        playA,
        playB,
        sessionA,
        options.SessionAStreamEndpoint,
        options.SessionBStreamEndpoint),
    "sm-d9-d11-d13" => RunD9D11D13Async(sessionA, options.SessionAStreamEndpoint),
    "sm-c1-c2" => RunC1C2Async(playA, playB),
    "sm-q9" => MultiNodeSpotRoutingProbe.RunAsync(multiA, multiB),
    "sm-f3-f5" => RunF3F5Async(playA, playB, gateway),
    "sm-e1-f4" => RunE1F4Async(playA, options.SessionAStreamEndpoint),
    "sm-e2-e3" => RunE2E3Async(playA, playB),
    "sm-a7-a8-c4" => RunA7A8C4Async(playA, playB, gateway),
    "sm-a3-a6-b4-b7" => RunA3A6B4B7Async(playA, playB, options.SessionAStreamEndpoint),
    "sm-a1-a2-a4-f1-f2" => RunA1A2A4F1F2Async(playA, playB),
    "sm-a12-a13" => RunA12A13Async(playA),
    "instance-track-a" => InstanceSpotTrackAScenario.RunAsync(playA, playB),
    "instance-idle" => InstanceSpotIdleEvictionScenario.RunAsync(playA),
    "instance-owner-loss" => InstanceSpotOwnerLossScenario.RunAsync(
        playA,
        playB,
        options.InstanceOwnerLossCrashAckFile
        ?? throw new InvalidOperationException(
            "Instance owner-loss crash acknowledgement file is required.")),
    "instance-queue-owner-loss" => InstanceSpotQueueOwnerLossScenario.RunAsync(
        playA,
        playB,
        options.InstanceOwnerLossCrashAckFile
        ?? throw new InvalidOperationException(
            "Instance queue owner-loss crash acknowledgement file is required."),
        options.InstanceOwnerLossRestartAckFile
        ?? throw new InvalidOperationException(
            "Instance queue owner-loss restart acknowledgement file is required.")),
    "instance-creating-join" => InstanceSpotCreatingJoinScenario.RunAsync(
        playA,
        playB,
        options.InstanceCreatingReleaseAckFile
        ?? throw new InvalidOperationException(
            "Instance creating release acknowledgement file is required.")),
    _ => throw new InvalidOperationException($"Unsupported SpotService operation group '{options.OperationGroup}'.")
});

Console.WriteLine($"spot-service client operation_group={options.OperationGroup} result=passed");

static async Task RunA1A2A4F1F2Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    try
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmA1EntrySpotRequestScenario.RunAsync(playA);
        await SmA4OwnerRoutingScenario.RunAsync(playA);
        await SmF1ClientServerChannelToSpotScenario.RunAsync(playA);
        await SmA2UserSpotStateMutationScenario.RunAsync(playA);
        await SmF2RouteMeshChannelToSpotScenario.RunAsync(playA, playB);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunA12A13Async(ZLinkHttpClient playA)
{
    await SmA12AutomaticSpotIdsScenario.RunAsync(playA);
    await SmA13SpotIdBoundaryScenario.RunAsync(playA);
}

static async Task RunG5Async(
    ZLinkHttpClient gateway,
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SmG5AAndG5BPlacementScenario.RunWeightDistributionAsync(gateway, playA, playB);
    await SmG5AAndG5BPlacementScenario.RunCapacityEligibilityAsync(gateway, playA, playB);
}

static async Task RunA3A6B4B7Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    string sessionAStreamEndpoint)
{
    try
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmA3SpecificSpotOwnerRoutingScenario.RunAsync(playA, playB);
        await SmA6SpotInitializeCloseLifecycleScenario.RunAsync(playA);

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 0, playBWeight: 100);
        await SmB4RemoteActorRequestReplyScenario.RunAsync(playB, sessionAStreamEndpoint);

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmB7ActorLifecycleOrderScenario.RunAsync(playA, sessionAStreamEndpoint);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunB1B2B3Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient sessionA,
    string sessionAStreamEndpoint)
{
    try
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmB1LocalActorJoinScenario.RunAsync(playA, sessionA, sessionAStreamEndpoint);

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 0, playBWeight: 100);
        await SmB2RemoteActorJoinScenario.RunAsync(playB, sessionA, sessionAStreamEndpoint);

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmB3RequestMessageFidelityScenario.RunAsync(playA, sessionAStreamEndpoint);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task SetPlacementWeightsAsync(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    int playAWeight,
    int playBWeight)
{
    await playA.Post("/placement-weight")
        .Body(new PlacementWeightReq(playAWeight))
        .Async<PlacementWeightRes>();
    await playB.Post("/placement-weight")
        .Body(new PlacementWeightReq(playBWeight))
        .Async<PlacementWeightRes>();

    // A weight of zero takes a node out of new placement (spec 24 §2.2), but
    // the call only sets it locally. Wait until each node reports the
    // availability that weight implies before placing anything, otherwise the
    // deciding node can still pick a node the scenario meant to exclude.
    await WaitPlacementAvailabilityAsync(playA, playAWeight > 0);
    await WaitPlacementAvailabilityAsync(playB, playBWeight > 0);
}

static async Task WaitPlacementAvailabilityAsync(
    ZLinkHttpClient play,
    bool expected)
{
    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
    while (true)
    {
        var snapshot = (await play.Get("/mesh-snapshot").Async<JsonElement>()).Body;
        if (snapshot.TryGetProperty("placement", out var placement)
            && placement.TryGetProperty("isAvailable", out var available)
            && available.GetBoolean() == expected)
            return;
        if (DateTimeOffset.UtcNow >= deadline)
            throw new InvalidOperationException(
                $"Placement availability did not reach {expected} within the setup window.");
        await Task.Delay(100);
    }
}

static async Task RunD2Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient sessionA,
    string sessionAStreamEndpoint)
{
    try
    {
        await SmD2RemoteActorSessionRelayScenario.RunAsync(
            sessionA,
            sessionAStreamEndpoint,
            () => SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0),
            () => SetPlacementWeightsAsync(playA, playB, playAWeight: 0, playBWeight: 100));
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunD2D6Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient sessionA,
    string sessionAStreamEndpoint,
    string sessionBStreamEndpoint)
{
    try
    {
        await SmD2RemoteActorSessionRelayScenario.RunAsync(
            sessionA,
            sessionAStreamEndpoint,
            () => SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0),
            () => SetPlacementWeightsAsync(playA, playB, playAWeight: 0, playBWeight: 100));

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmD6BoundSessionPushTargetingScenario.RunAsync(
            sessionAStreamEndpoint,
            sessionBStreamEndpoint,
            () => SetPlacementWeightsAsync(
                playA,
                playB,
                playAWeight: 0,
                playBWeight: 100));
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunC1Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmC1ChannelToSpotMessagingScenario.RunAsync(playA);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunC2Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmC2SpotToChannelMessagingScenario.RunAsync(playA, playB);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunC1C2Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmC1ChannelToSpotMessagingScenario.RunAsync(playA);
        await SmC2SpotToChannelMessagingScenario.RunAsync(playA, playB);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunE1F4Async(ZLinkHttpClient playA, string sessionAStreamEndpoint)
{
    await SmE1MissingSpotRouteScenario.RunAsync(playA, sessionAStreamEndpoint);
    await SmF4MissingTargetSpotRouteScenario.RunAsync(playA);
}

static async Task RunE2E3Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmE2SpotTimerTickScenario.RunAsync(playA);
        await SmE3IdleTimerSpotCloseScenario.RunAsync(playA);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunE4Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmE4TimerOverrunPolicyScenario.RunAsync(playA);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunA7A8C4Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient gateway)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmA7SpotTypeMismatchScenario.RunAsync(playA);
        await SmA8WorkerOffloadScenario.RunAsync(playA);
        await SmC4SpotPublisherClientScenario.RunAsync(playA, gateway);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunD9D11D13Async(ZLinkHttpClient sessionA, string sessionAStreamEndpoint)
{
    await SmD9StreamInboundObserverScenario.RunAsync(sessionAStreamEndpoint);
    await SmD11StreamAndRouteRequestScenario.RunAsync(sessionA, sessionAStreamEndpoint);
    await SmD13HeartbeatRequestScenario.RunAsync(sessionAStreamEndpoint);
}

static async Task RunF3F5Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient gateway)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmF3MixedRouteAndSpotEgressScenario.RunAsync(playA, gateway);
        await SmF5ClosedSpotRouteIsolationScenario.RunAsync(playA, gateway);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}

static async Task RunF3Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient gateway)
{
    await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
    try
    {
        await SmF3MixedRouteAndSpotEgressScenario.RunAsync(playA, gateway);
    }
    finally
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 100);
    }
}
