// Verifies SM-D4A exact rebind and stale binding isolation.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD4AStaleBindingIsolationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient gateway,
        string sessionAStreamEndpoint,
        string sessionBStreamEndpoint)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"actor-sm-d4a-{suffix}";
        var sessionACompanionId = $"actor-sm-d4a-a-companion-{suffix}";
        var sessionBCompanionId = $"actor-sm-d4a-b-companion-{suffix}";
        await using var sessionA = CreateConnector(sessionAStreamEndpoint);
        await using var sessionB = CreateConnector(sessionBStreamEndpoint);
        await sessionA.Connect.Async();
        await sessionB.Connect.Async();

        await BindPairAsync(sessionA, actorId, sessionACompanionId);
        var before = await PingAsync(sessionA, actorId, "before-rebind");
        var beforeRef = await CaptureRefAsync(gateway, actorId);

        await BindPairAsync(sessionB, actorId, sessionBCompanionId);
        var currentBeforeProbe = await PingAsync(sessionB, actorId, "current-before-stale");

        var stale = await sessionA
            .Request(new StaleBindingProbeReq(actorId, "must-not-reach-current-binding"))
            .PacketName("StaleBindingProbeReq")
            .Async<StaleBindingProbeRes>();
        ZlinkStreamAssert.Ensure(
            stale.ActorId == actorId
            && stale.RelayRejected
            && stale.DisconnectCompleted
            && string.Equals(
                stale.ErrorKind,
                ZLinkFrameworkErrorKind.InvalidOperation.ToString(),
                StringComparison.Ordinal),
            $"SM-D4A expected a typed stale relay result, got '{stale.ErrorKind}'.");

        var sessionACompanion = await PingAsync(
            sessionA,
            sessionACompanionId,
            "session-a-companion-current");
        ZlinkStreamAssert.Ensure(
            sessionACompanion.ActorId == sessionACompanionId,
            "SM-D4A stale Actor X lifecycle changed Session A's companion binding.");

        // Closing Session A submits its delayed connection cleanup after Session B
        // already owns both exact bindings.
        await sessionA.Close.Async();

        var currentAfterProbe = await PingAsync(sessionB, actorId, "current-after-stale");
        ZlinkStreamAssert.Ensure(
            currentAfterProbe.Seen == currentBeforeProbe.Seen + 1,
            "SM-D4A stale Session A relay reached Actor X after Session B rebind.");
        var companion = await PingAsync(
            sessionB,
            sessionBCompanionId,
            "session-b-companion-current");
        ZlinkStreamAssert.Ensure(
            companion.ActorId == sessionBCompanionId,
            "SM-D4A Session A late disconnect changed the companion binding.");

        var afterRef = await CaptureRefAsync(gateway, actorId);
        ZlinkStreamAssert.Ensure(
            beforeRef.ActorId == afterRef.ActorId
            && beforeRef.Generation == afterRef.Generation,
            "SM-D4A rebind changed Actor X ObjectGeneration.");
        ZlinkStreamAssert.Ensure(
            before.SpotRid == currentAfterProbe.SpotRid,
            "SM-D4A rebind changed Actor X Spot membership.");

        var evidence = (await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq([]))
                .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.All(line =>
                !line.Contains($"entry-disconnected|", StringComparison.Ordinal)
                || !line.Contains($"actor={actorId}", StringComparison.Ordinal)),
            "SM-D4A stale lifecycle cleanup invoked a disconnect callback for a current binding.");

        Console.WriteLine("operation SpotService.sm-d4a passed");
    }

    private static async Task BindPairAsync(
        IZlinkStreamConnector session,
        string actorId,
        string companionId)
    {
        var result = await session
            .Request(new MultiBindReq(actorId, companionId))
            .PacketName("MultiBindReq")
            .Async<MultiBindRes>();
        ZlinkStreamAssert.Ensure(result.BoundCount == 2, "SM-D4A expected two current bindings.");
    }

    private static async Task<ActorPingRes> PingAsync(
        IZlinkStreamConnector session,
        string actorId,
        string value)
    {
        var result = await session.Request(new ActorPingReq(value))
            .PacketName("ActorPingReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, actorId)
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(
            result.ActorId == actorId && result.Value == value,
            $"SM-D4A Actor relay mismatch for '{actorId}'.");
        return result;
    }

    private static async Task<ActorRefRes> CaptureRefAsync(
        ZLinkHttpClient gateway,
        string actorId) =>
        (await gateway.Post("/actor/capture-ref")
            .Body(new ActorRefReq(actorId))
            .Async<ActorRefRes>()).Body;

    private static IZlinkStreamConnector CreateConnector(string endpoint) =>
        ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
}
