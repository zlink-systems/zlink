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
    "sm-a11" => SmA11ReservedEntrySpotIdScenario.RunAsync(playA),
    "sm-b1" => SmB1LocalActorJoinScenario.RunAsync(playA, sessionA, options.SessionAStreamEndpoint),
    "sm-b0" => SmB0ActorManagerLifecycleScenario.RunAsync(gateway, playA, playB),
    "sm-b2" => SmB2RemoteActorJoinScenario.RunAsync(playB, sessionA, options.SessionAStreamEndpoint),
    "sm-b3" => SmB3RequestMessageFidelityScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-b4" => SmB4RemoteActorRequestReplyScenario.RunAsync(playB, options.SessionAStreamEndpoint),
    "sm-b5" => SmB5MissingActorPacketScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-b6" => SmB6ActorDisconnectCallbackScenario.RunAsync(playA, playB, sessionA, options.SessionAStreamEndpoint),
    "sm-b7" => SmB7ActorLifecycleOrderScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-b8" => SmB8ExplicitActorDestroyScenario.RunAsync(gateway),
    "sm-b9" => SmB9ActorJoinAdmissionScenario.RunAsync(playA, playB, options.SessionAStreamEndpoint),
    "sm-c1" => SmC1ChannelToSpotMessagingScenario.RunAsync(playA),
    "sm-c2" => SmC2SpotToChannelMessagingScenario.RunAsync(playA),
    "sm-c3" => SmC3SpotMeshMessagingScenario.RunAsync(playA),
    "sm-c4" => SmC4SpotPublisherClientScenario.RunAsync(playA, gateway),
    "sm-c5" => SmC5RemoteSpotPublishSubscribeScenario.RunAsync(playA, playB),
    "sm-c6" => SmC6LogicalMulticastBackpressureScenario.RunAsync(
        gateway,
        playA,
        playB,
        options.SmC6PauseAckFile,
        options.SmC6ResumeAckFile,
        options.SmC6BlockingPauseAckFile),
    "sm-d1" => SmD1LocalActorSessionRelayScenario.RunAsync(sessionA, options.SessionAStreamEndpoint),
    "sm-d2" => SmD2RemoteActorSessionRelayScenario.RunAsync(sessionA, options.SessionAStreamEndpoint),
    "sm-d3" => SmD3EntryAndUserSpotSessionRelayScenario.RunAsync(playA, options.SessionAStreamEndpoint),
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
    "sm-d8" => SmD8StreamReconnectRecoveryScenario.RunAsync(playA, options.SessionAStreamEndpoint),
    "sm-d9" => SmD9StreamInboundObserverScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d10" => SmD10BoundedSessionBackpressureScenario.RunAsync(options.SessionAStreamEndpoint, options.SessionBStreamEndpoint),
    "sm-d11" => SmD11StreamAndRouteRequestScenario.RunAsync(sessionA, options.SessionAStreamEndpoint),
    "sm-d12" => SmD12SessionReconnectMigrationScenario.RunAsync(options.SessionAStreamEndpoint, options.SessionBStreamEndpoint),
    "sm-d13" => SmD13HeartbeatRequestScenario.RunAsync(options.SessionAStreamEndpoint),
    "sm-d14" => SmD14TlsStreamValidationScenario.RunAsync(options.SessionATlsStreamEndpoint),
    "sm-d15" => SmD15GatewayActorSessionPushScenario.RunAsync(playA, gateway, options.SessionAStreamEndpoint),
    "sm-e1" => SmE1MissingSpotRouteScenario.RunAsync(playA),
    "sm-e2" => SmE2SpotTimerTickScenario.RunAsync(playA),
    "sm-e3" => SmE3IdleTimerSpotCloseScenario.RunAsync(playA),
    "sm-e4" => SmE4TimerOverrunPolicyScenario.RunAsync(playA),
    "sm-f1" => SmF1ClientServerChannelToSpotScenario.RunAsync(playA),
    "sm-f2" => SmF2RouteMeshChannelToSpotScenario.RunAsync(playA, playB),
    "sm-f3" => RunF3Async(playA, gateway),
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
    "sm-b1-b2-b3-b5" => RunB1B2B3B5Async(playA, playB, sessionA, options.SessionAStreamEndpoint),
    "sm-d1-d6" => RunD1D2D6Async(
        playA,
        playB,
        sessionA,
        options.SessionAStreamEndpoint,
        options.SessionBStreamEndpoint),
    "sm-d9-d11-d13" => RunD9D11D13Async(sessionA, options.SessionAStreamEndpoint),
    "sm-c1-c2" => RunC1C2Async(playA),
    "sm-q9" => MultiNodeSpotRoutingProbe.RunAsync(multiA, multiB),
    "sm-f3-f5" => RunF3F5Async(playA, gateway),
    "sm-e1-f4" => RunE1F4Async(playA),
    "sm-e2-e3" => RunE2E3Async(playA),
    "sm-a7-a8-c4" => RunA7A8C4Async(playA, gateway),
    "sm-a3-a6-b4-b7" => RunA3A6B4B7Async(playA, playB, options.SessionAStreamEndpoint),
    "sm-a1-a2-a4-f1-f2" => RunA1A2A4F1F2Async(playA, playB),
    "instance-track-a" => InstanceSpotTrackAScenario.RunAsync(playA, playB),
    "instance-idle" => InstanceSpotIdleEvictionScenario.RunAsync(playA),
    _ => throw new InvalidOperationException($"Unsupported SpotService operation group '{options.OperationGroup}'.")
});

Console.WriteLine($"spot-service client operation_group={options.OperationGroup} result=passed");

static async Task RunA1A2A4F1F2Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB)
{
    await SmA1EntrySpotRequestScenario.RunAsync(playA);
    await SmA4OwnerRoutingScenario.RunAsync(playA);
    await SmF1ClientServerChannelToSpotScenario.RunAsync(playA);
    await SmF2RouteMeshChannelToSpotScenario.RunAsync(playA, playB);
    await SmA2UserSpotStateMutationScenario.RunAsync(playA);
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

static async Task RunB1B2B3B5Async(
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
        await SmB5MissingActorPacketScenario.RunAsync(playA, sessionAStreamEndpoint);
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

static async Task RunD1D2D6Async(
    ZLinkHttpClient playA,
    ZLinkHttpClient playB,
    ZLinkHttpClient sessionA,
    string sessionAStreamEndpoint,
    string sessionBStreamEndpoint)
{
    try
    {
        await SetPlacementWeightsAsync(playA, playB, playAWeight: 100, playBWeight: 0);
        await SmD1LocalActorSessionRelayScenario.RunAsync(sessionA, sessionAStreamEndpoint);

        await SetPlacementWeightsAsync(playA, playB, playAWeight: 0, playBWeight: 100);
        await SmD2RemoteActorSessionRelayScenario.RunAsync(sessionA, sessionAStreamEndpoint);

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

static async Task RunC1C2Async(ZLinkHttpClient playA)
{
    await SmC1ChannelToSpotMessagingScenario.RunAsync(playA);
    await SmC2SpotToChannelMessagingScenario.RunAsync(playA);
}

static async Task RunE1F4Async(ZLinkHttpClient playA)
{
    await SmE1MissingSpotRouteScenario.RunAsync(playA);
    await SmF4MissingTargetSpotRouteScenario.RunAsync(playA);
}

static async Task RunE2E3Async(ZLinkHttpClient playA)
{
    await SmE2SpotTimerTickScenario.RunAsync(playA);
    await SmE3IdleTimerSpotCloseScenario.RunAsync(playA);
}

static async Task RunA7A8C4Async(ZLinkHttpClient playA, ZLinkHttpClient gateway)
{
    await SmA7SpotTypeMismatchScenario.RunAsync(playA);
    await SmA8WorkerOffloadScenario.RunAsync(playA);
    await SmC4SpotPublisherClientScenario.RunAsync(playA, gateway);
}

static async Task RunD9D11D13Async(ZLinkHttpClient sessionA, string sessionAStreamEndpoint)
{
    await SmD9StreamInboundObserverScenario.RunAsync(sessionAStreamEndpoint);
    await SmD11StreamAndRouteRequestScenario.RunAsync(sessionA, sessionAStreamEndpoint);
    await SmD13HeartbeatRequestScenario.RunAsync(sessionAStreamEndpoint);
}

static async Task RunF3F5Async(ZLinkHttpClient playA, ZLinkHttpClient gateway)
{
    await SmF3MixedRouteAndSpotEgressScenario.RunAsync(playA, gateway);
    await SmF5ClosedSpotRouteIsolationScenario.RunAsync(playA, gateway);
}

static async Task RunF3Async(ZLinkHttpClient playA, ZLinkHttpClient gateway)
{
    await SmF3MixedRouteAndSpotEgressScenario.RunAsync(playA, gateway);
}
