using Zlink.Framework.E2E.Configuration;
using System.Diagnostics;
using System.Net.Http.Json;
using SpotActorTransfer.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Support;

internal sealed class SpotActorTransferScenarioContext : IDisposable
{
    private readonly SemaphoreSlim _placementGate = new(1, 1);
    private readonly HttpClient _transportProxyClient = new()
    {
        Timeout = TimeSpan.FromSeconds(15)
    };

    public SpotActorTransferScenarioContext(ClientOptions options)
    {
        Options = options;
        NodeA = CreateClient(options.NodeAUrl);
        NodeB = CreateClient(options.NodeBUrl);
        NodeC = CreateClient(options.NodeCUrl);
        NodeD = CreateClient(options.NodeDUrl);
    }

    public ClientOptions Options { get; }
    public ZLinkHttpClient NodeA { get; }
    public ZLinkHttpClient NodeB { get; }
    public ZLinkHttpClient NodeC { get; }
    public ZLinkHttpClient NodeD { get; }

    public async Task WaitMeshReadyAsync()
    {
        var expected = new[]
        {
            (Node: NodeA, Peers: new[] { "actor-b", "actor-c", "actor-d", "session-a", "session-b" }),
            (Node: NodeB, Peers: new[] { "actor-a", "actor-c", "actor-d", "session-a", "session-b" }),
            (Node: NodeC, Peers: new[] { "actor-a", "actor-b", "actor-d", "session-a", "session-b" }),
            (Node: NodeD, Peers: new[] { "actor-a", "actor-b", "actor-c" })
        };
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        var observed = new List<string>();
        do
        {
            var ready = true;
            observed.Clear();
            foreach (var (node, peers) in expected)
            {
                var snapshot = (await node.Get("/mesh/ready").Async<MeshReadyRes>()).Body;
                observed.Add(
                    $"{snapshot.NodeRid}:peers={string.Join(',', snapshot.ReadyPeerRids)}"
                    + $";spots={string.Join(',', snapshot.ReadySpotTypes)}");
                if (peers.Any(peer => !snapshot.ReadyPeerRids.Any(
                        rid => IsNode(rid, peer))))
                {
                    ready = false;
                }
                if (!snapshot.ReadySpotTypes.Contains(
                        SpotActorTransferNames.UserSpotType,
                        StringComparer.Ordinal))
                {
                    ready = false;
                }
            }

            if (ready) return;
            await Task.Delay(100);
        } while (DateTimeOffset.UtcNow < deadline);

        throw new TimeoutException(
            "SpotActorTransfer RouteMesh peers did not become ready: "
            + string.Join(" | ", observed));
    }

    public void Dispose()
    {
        _placementGate.Dispose();
        _transportProxyClient.Dispose();
        NodeA.Dispose();
        NodeB.Dispose();
        NodeC.Dispose();
        NodeD.Dispose();
    }

    public static bool IsNode(string actualRid, string diagnosticPrefix) =>
        actualRid.StartsWith(
            diagnosticPrefix + "-",
            StringComparison.Ordinal);

    public async Task<CreateSpotRes> CreateSpotAsync(
        ZLinkHttpClient placementNode,
        string spotId,
        string mode = "accept")
    {
        return await WithPlacementNodeAsync(
            placementNode,
            async () =>
            {
                var result = (await NodeA.Post("/spots")
                                 .Body(new CreateSpotReq(spotId, mode))
                                 .Async<CreateSpotRes>()).Body
                             ?? throw new InvalidOperationException(
                                 "Create spot response was null.");
                EnsurePlacementOwner(placementNode, result.NodeRid, "Spot");
                return result;
            });
    }

    public async Task<ActorCreateRes> CreateActorAsync(
        ZLinkHttpClient placementNode,
        string actorId,
        string actorType,
        int stateVersion,
        int applicationStateBytes = 0)
    {
        return await WithPlacementNodeAsync(
            placementNode,
            async () =>
            {
                var result = (await NodeA.Post("/actors").Body(new ActorCreateReq(
                                   actorId,
                                   actorType,
                                   stateVersion,
                                   applicationStateBytes))
                               .Async<ActorCreateRes>()).Body
                             ?? throw new InvalidOperationException(
                                 "Create actor response was null.");
                EnsurePlacementOwner(placementNode, result.NodeRid, "Actor");
                return result;
            });
    }

    internal async Task<T> WithPlacementNodeAsync<T>(
        ZLinkHttpClient placementNode,
        Func<Task<T>> action)
    {
        await _placementGate.WaitAsync();
        try
        {
            foreach (var node in ActorNodes())
                await SetPlacementWeightAsync(node, 0);
            await SetPlacementWeightAsync(placementNode, 100);
            await WaitForExclusivePlacementAsync(placementNode);
            return await action();
        }
        finally
        {
            try
            {
                foreach (var node in ActorNodes())
                    await SetPlacementWeightAsync(node, 100);
            }
            finally
            {
                _placementGate.Release();
            }
        }
    }

    private async Task SetPlacementWeightAsync(
        ZLinkHttpClient node,
        int weight)
    {
        var result = (await node.Post("/placement-weight")
                         .Body(new PlacementWeightReq(weight))
                         .Async<PlacementWeightRes>()).Body
                     ?? throw new InvalidOperationException(
                         "Placement weight response was null.");
        ZlinkStreamAssert.Ensure(
            result.Weight == weight,
            $"Placement weight expected {weight}, got {result.Weight}.");
    }

    public async Task SetExclusivePlacementAsync(
        ZLinkHttpClient placementNode)
    {
        foreach (var node in ActorNodes())
            await SetPlacementWeightAsync(
                node,
                ReferenceEquals(node, placementNode) ? 100 : 0);
        await WaitForExclusivePlacementAsync(placementNode);
    }

    private async Task WaitForExclusivePlacementAsync(
        ZLinkHttpClient placementNode)
    {
        const int requiredConsecutiveMatches = 4;
        var expectedPrefix = NodePrefix(placementNode);
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        var consecutiveMatches = 0;
        var observed = new List<string>();
        while (DateTimeOffset.UtcNow < deadline)
        {
            var probeId =
                $"placement-probe-{Guid.NewGuid():N}";
            RelocationBulkSpotCreateRes created;
            try
            {
                created = await CreateBulkSpotsAsync(
                    NodeA,
                    new RelocationBulkSpotCreateReq(
                        "PLACEMENT-PROPAGATION-PROBE",
                        probeId,
                        Count: 1,
                        ApplicationStateBytes: 0,
                        InstanceSpot: false,
                        MaxConcurrency: 1),
                    TimeSpan.FromSeconds(5));
            }
            catch (ZLinkFrameworkException exception)
            {
                observed.Add($"transient:{exception.Kind}");
                consecutiveMatches = 0;
                await Task.Delay(50);
                continue;
            }
            var spotId = AssertSingle(created.SpotIds, "Spot ID");
            var nodeRid = AssertSingle(created.NodeRids, "owner");
            observed.Add(nodeRid);
            try
            {
                consecutiveMatches = IsNode(nodeRid, expectedPrefix)
                    ? consecutiveMatches + 1
                    : 0;
                if (consecutiveMatches >= requiredConsecutiveMatches)
                    return;
            }
            finally
            {
                _ = await NodeA
                    .Post($"/payload-spots/{spotId}/close")
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<bool>();
            }
            await Task.Delay(20);
        }

        throw new TimeoutException(
            $"Placement did not converge on {expectedPrefix}; "
            + $"observed owners: {string.Join(',', observed)}.");

        static string AssertSingle(
            IReadOnlyList<string> values,
            string valueName) =>
            values.Count == 1
                ? values[0]
                : throw new InvalidOperationException(
                    $"Placement probe returned {values.Count} {valueName} values.");
    }

    public async Task RestoreDefaultPlacementAsync(
        params ZLinkHttpClient[] excludedNodes)
    {
        foreach (var node in ActorNodes())
        {
            if (excludedNodes.Any(excluded =>
                    ReferenceEquals(excluded, node)))
                continue;
            await SetPlacementWeightAsync(node, 100);
        }
    }

    public async Task<RelocationBulkActorCreateRes> CreateBulkActorsAsync(
        ZLinkHttpClient coordinator,
        RelocationBulkActorCreateReq request) =>
        (await coordinator.Post("/workload/actors/create-bulk")
                .Body(request)
                .Timeout(TimeSpan.FromMinutes(5))
                .Async<RelocationBulkActorCreateRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Bulk Actor creation response was null.");

    public async Task<RelocationBulkSpotCreateRes> CreateBulkSpotsAsync(
        ZLinkHttpClient coordinator,
        RelocationBulkSpotCreateReq request,
        TimeSpan? timeout = null) =>
        (await coordinator.Post("/workload/spots/create-bulk")
                .Body(request)
                .Timeout(timeout ?? TimeSpan.FromMinutes(5))
                .Async<RelocationBulkSpotCreateRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Bulk Spot creation response was null.");

    private void EnsurePlacementOwner(
        ZLinkHttpClient placementNode,
        string actualNodeRid,
        string objectKind)
    {
        var expectedPrefix = NodePrefix(placementNode);
        ZlinkStreamAssert.Ensure(
            IsNode(actualNodeRid, expectedPrefix),
            $"{objectKind} placement expected {expectedPrefix}, got {actualNodeRid}.");
    }

    private string NodePrefix(ZLinkHttpClient placementNode) =>
        ReferenceEquals(placementNode, NodeA)
            ? "actor-a"
            : ReferenceEquals(placementNode, NodeB)
                ? "actor-b"
                : ReferenceEquals(placementNode, NodeC)
                    ? "actor-c"
                    : throw new InvalidOperationException(
                        "Placement node must be one of the scenario Actor nodes.");

    private IEnumerable<ZLinkHttpClient> ActorNodes()
    {
        yield return NodeA;
        yield return NodeB;
        yield return NodeC;
    }

    public ZLinkHttpClient NodeForRid(string nodeRid)
    {
        if (IsNode(nodeRid, "actor-a")) return NodeA;
        if (IsNode(nodeRid, "actor-b")) return NodeB;
        if (IsNode(nodeRid, "actor-c")) return NodeC;
        throw new InvalidOperationException(
            $"Unknown Actor node RID '{nodeRid}'.");
    }

    public (ZLinkHttpClient Client, string DiagnosticPrefix) OtherActorNode(
        string sourceNodeRid)
    {
        return IsNode(sourceNodeRid, "actor-a")
            ? (NodeB, "actor-b")
            : (NodeA, "actor-a");
    }

    public ZLinkHttpClient ThirdActorNode(
        ZLinkHttpClient first,
        ZLinkHttpClient second)
    {
        foreach (var candidate in new[] { NodeA, NodeB, NodeC })
            if (!ReferenceEquals(candidate, first)
                && !ReferenceEquals(candidate, second))
                return candidate;
        throw new InvalidOperationException(
            "A third Actor node is required for stale-route observation.");
    }

    public async Task<GateReleaseRes> ReleaseJoinedGateAsync(ZLinkHttpClient client, string spotId)
    {
        return (await client.Post($"/joined-gates/{spotId}/release").Async<GateReleaseRes>()).Body
               ?? throw new InvalidOperationException("Gate release response was null.");
    }

    public async Task<GateReleaseRes> ReleaseTransferGateAsync(
        ZLinkHttpClient client,
        string actorId)
    {
        return (await client.Post($"/transfer-gates/{actorId}/release")
                   .Async<GateReleaseRes>()).Body
               ?? throw new InvalidOperationException(
                   "Transfer gate release response was null.");
    }

    public async Task ArmCleanupGateAsync(
        ZLinkHttpClient client,
        string actorId,
        string scenario)
    {
        var result = (await client.Post($"/cleanup-gates/{actorId}/arm")
                .Body(new CleanupGateArmReq(scenario))
                .Async<CleanupGateRes>()).Body
            ?? throw new InvalidOperationException("Cleanup gate arm response was null.");
        ZlinkStreamAssert.Ensure(result.Changed,
            $"{scenario} cleanup gate for actor '{actorId}' was already armed.");
    }

    public async Task ReleaseCleanupGateAsync(
        ZLinkHttpClient client,
        string actorId,
        string scenario)
    {
        var result = (await client.Post($"/cleanup-gates/{actorId}/release")
                .Async<CleanupGateRes>()).Body
            ?? throw new InvalidOperationException("Cleanup gate release response was null.");
        ZlinkStreamAssert.Ensure(result.Changed,
            $"{scenario} cleanup gate for actor '{actorId}' was not waiting.");
    }

    public async Task AllowCleanupAttemptAsync(
        ZLinkHttpClient client,
        string actorId,
        string scenario)
    {
        var result = (await client.Post($"/cleanup-gates/{actorId}/allow-attempt")
                .Async<CleanupGateRes>()).Body
            ?? throw new InvalidOperationException("Cleanup gate allow response was null.");
        ZlinkStreamAssert.Ensure(result.Changed,
            $"{scenario} cleanup attempt for actor '{actorId}' was already allowed.");
    }

    public async Task<ActorRefRes> GetActorRefAsync(ZLinkHttpClient client, string actorId)
    {
        return (await client.Get($"/actors/{actorId}/ref").Async<ActorRefRes>()).Body
               ?? throw new InvalidOperationException("Actor ref response was null.");
    }

    public async Task<ActorRefRes> WaitActorOwnerAsync(
        ZLinkHttpClient client,
        string actorId,
        string expectedNodeRid,
        TimeSpan? timeout = null)
    {
        var deadline = DateTimeOffset.UtcNow
                       + (timeout ?? TimeSpan.FromSeconds(5));
        ActorRefRes? current = null;
        do
        {
            try
            {
                current = await GetActorRefAsync(client, actorId);
                if (string.Equals(
                        current.NodeRid,
                        expectedNodeRid,
                        StringComparison.Ordinal))
                    return current;
            }
            catch (ZLinkFrameworkException)
            {
                // The target callback can complete just before its public
                // location view observes the committed owner.
            }

            await Task.Delay(TimeSpan.FromMilliseconds(20));
        } while (DateTimeOffset.UtcNow < deadline);

        throw new InvalidOperationException(
            $"Actor '{actorId}' owner expected '{expectedNodeRid}', "
            + $"got '{current?.NodeRid ?? "<not-found>"}'.");
    }

    public async Task<ActorDestroyRes> DestroyActorAsync(
        ZLinkHttpClient client,
        string actorId)
    {
        return (await client.Post($"/actors/{actorId}/destroy")
                   .Async<ActorDestroyRes>()).Body
               ?? throw new InvalidOperationException("Actor destroy response was null.");
    }

    public async Task<ActorRefRes> GetActorRefWithEvidenceAsync(
        ZLinkHttpClient client,
        string actorId,
        string scenario,
        string marker)
    {
        return (await client.Get($"/actors/{actorId}/ref-evidence/{scenario}/{marker}")
                   .Async<ActorRefRes>()).Body
               ?? throw new InvalidOperationException("Actor ref evidence response was null.");
    }

    public async Task<IReadOnlyList<ActorEvidence>> GetEvidenceAsync(ZLinkHttpClient client)
    {
        return (await client.Get("/evidence").Async<IReadOnlyList<ActorEvidence>>()).Body
               ?? throw new InvalidOperationException("Evidence response was null.");
    }

    public async Task<IReadOnlyList<RelocationBlobMeasurement>>
        GetRelocationBlobMeasurementsAsync(ZLinkHttpClient client)
    {
        return (await client.Get("/relocation-blobs")
                   .Async<IReadOnlyList<RelocationBlobMeasurement>>()).Body
               ?? throw new InvalidOperationException(
                   "Relocation blob measurement response was null.");
    }

    public async Task ResetRelocationBlobMeasurementsAsync(
        params ZLinkHttpClient[] clients)
    {
        foreach (var client in clients)
            await client.Post("/relocation-blobs/reset").AsyncRaw();
    }

    public async Task<RelocationPayloadSpotRes> CreatePayloadUserSpotAsync(
        ZLinkHttpClient coordinator,
        string spotId,
        RelocationPayloadSpotReq request) =>
        (await coordinator.Post($"/payload-spots/user/{spotId}")
                .Body(request)
                .Async<RelocationPayloadSpotRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Payload User Spot response was null.");

    public async Task<RelocationPayloadSpotRes> ActivatePayloadInstanceSpotAsync(
        ZLinkHttpClient coordinator,
        string spotId,
        RelocationPayloadSpotReq request) =>
        (await coordinator.Post($"/payload-spots/instance/{spotId}")
                .Body(request)
                .Async<RelocationPayloadSpotRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Payload Instance Spot response was null.");

    public async Task ClosePayloadSpotAsync(
        ZLinkHttpClient coordinator,
        string spotId)
    {
        var closed = (await coordinator
                .Post($"/payload-spots/{spotId}/close")
                .Async<bool>())
            .Body;
        ZlinkStreamAssert.Ensure(
            closed,
            $"Payload Spot '{spotId}' was not closed.");
    }

    public async Task<RelocateHostRes> RelocateAsync(
        ZLinkHttpClient source,
        TimeSpan? deadline = null) =>
        (await source.Post("/relocate")
                .Body(new RelocateHostReq(
                    DeadlineMilliseconds: checked((int)(
                        deadline ?? TimeSpan.FromMinutes(2))
                        .TotalMilliseconds)))
                .Timeout(deadline ?? TimeSpan.FromMinutes(2))
                .Async<RelocateHostRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Host relocation response was null.");

    public async Task<RelocationWorkloadReply> RequestActorWorkloadAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        (await submittingNode.Post("/workload/actors/request")
                .Body(request)
                .Timeout(TimeSpan.FromMilliseconds(
                    request.TimeoutMilliseconds))
                .Async<RelocationWorkloadReply>())
            .Body
        ?? throw new InvalidOperationException(
            "Actor workload reply was null.");

    public async Task<RelocationWorkloadProbeRes> RequestActorWorkloadProbeAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        (await submittingNode.Post("/workload/actors/request-probe")
                .Body(request)
                .Timeout(TimeSpan.FromMilliseconds(
                    request.TimeoutMilliseconds))
                .Async<RelocationWorkloadProbeRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Actor workload probe response was null.");

    public Task SendActorWorkloadAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        submittingNode.Post("/workload/actors/send")
            .Body(request)
            .AsyncRaw()
            .AsTask();

    public async Task<RelocationQueueBlockRes> BlockActorQueueAsync(
        ZLinkHttpClient submittingNode,
        RelocationQueueBlockReq request) =>
        (await submittingNode.Post("/workload/actors/block")
                .Body(request)
                .Timeout(TimeSpan.FromSeconds(35))
                .Async<RelocationQueueBlockRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Actor queue blocker response was null.");

    public async Task<RelocationWorkloadReply> RequestSpotWorkloadAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        (await submittingNode.Post("/workload/spots/request")
                .Body(request)
                .Timeout(TimeSpan.FromMilliseconds(
                    request.TimeoutMilliseconds))
                .Async<RelocationWorkloadReply>())
            .Body
        ?? throw new InvalidOperationException(
            "Spot workload reply was null.");

    public async Task<RelocationWorkloadProbeRes> RequestSpotWorkloadProbeAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        (await submittingNode.Post("/workload/spots/request-probe")
                .Body(request)
                .Timeout(TimeSpan.FromMilliseconds(
                    request.TimeoutMilliseconds))
                .Async<RelocationWorkloadProbeRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Spot workload probe response was null.");

    public Task SendSpotWorkloadAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadCallReq request) =>
        submittingNode.Post("/workload/spots/send")
            .Body(request)
            .AsyncRaw()
            .AsTask();

    public async Task<RelocationQueueBlockRes> BlockSpotQueueAsync(
        ZLinkHttpClient submittingNode,
        RelocationQueueBlockReq request) =>
        (await submittingNode.Post("/workload/spots/block")
                .Body(request)
                .Timeout(TimeSpan.FromSeconds(35))
                .Async<RelocationQueueBlockRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Spot queue blocker response was null.");

    public async Task<RelocationReadySignalRes> SignalRelocationReadyAsync(
        ZLinkHttpClient submittingNode,
        RelocationWorkloadReadyCallReq request) =>
        (await submittingNode.Post("/workload/spots/relocation-ready")
                .Body(request)
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<RelocationReadySignalRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Relocation readiness response was null.");

    public async Task<IReadOnlyList<RelocationLocationSnapshot>>
        GetRelocationLocationsAsync(
            ZLinkHttpClient coordinator,
            IReadOnlyCollection<string> actorIds,
            IReadOnlyCollection<string> spotIds) =>
        (await coordinator.Post("/workload/locations")
                .Body(new RelocationLocationQueryReq(
                    actorIds.ToArray(),
                    spotIds.ToArray()))
                .Timeout(TimeSpan.FromMinutes(2))
                .Async<IReadOnlyList<RelocationLocationSnapshot>>())
            .Body
        ?? throw new InvalidOperationException(
            "Relocation location response was null.");

    public async Task<IReadOnlyList<RelocationMessageFlowEvidence>>
        GetRelocationMessageFlowsAsync(ZLinkHttpClient node) =>
        (await node.Get("/workload/message-flow")
                .Async<IReadOnlyList<RelocationMessageFlowEvidence>>())
            .Body
        ?? throw new InvalidOperationException(
            "Relocation message-flow evidence response was null.");

    public async Task<ProcessMemoryRes> GetProcessMemoryAsync(
        ZLinkHttpClient client) =>
        (await client.Get("/process-memory").Async<ProcessMemoryRes>())
            .Body
        ?? throw new InvalidOperationException(
            "Process memory response was null.");

    public async Task ShutdownAsync(ZLinkHttpClient client)
    {
        await client.Post("/shutdown").AsyncRaw();
    }

    public async Task ShutdownAndWaitUnavailableAsync(ZLinkHttpClient client, string url)
    {
        await ShutdownAsync(client);
        await WaitUnavailableAsync(url, "shutdown");
    }

    public async Task CrashNodeAAndWaitUnavailableAsync()
    {
        using var process = Process.GetProcessById(Options.NodeAPid);
        process.Kill(entireProcessTree: true);
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        await WaitUnavailableAsync(Options.NodeAUrl, "SIGKILL");
    }

    private static async Task WaitUnavailableAsync(string url, string operation)
    {
        using var probe = ZLinkHttpClient.Create(url)
            .Timeout(TimeSpan.FromMilliseconds(250))
            .Build();
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                await probe.Get("/health").AsyncRaw();
            }
            catch (Exception error) when (error is HttpRequestException or IOException)
            {
                return;
            }
            //  A refused or dropped connection is the proof this loop waits
            //  for, whatever kind the client stamps on it. ZLinkHttpClient maps
            //  connection failures to Unavailable (see RetryPolicy), so keying
            //  on InternalFailure let the very exception we wanted escape.
            catch (ZLinkFrameworkException error) when (HasConnectionFailure(error))
            {
                return;
            }
            catch (Exception error) when (
                error is TaskCanceledException or TimeoutException
                || error is ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.InternalFailure })
            {
                // A slow probe does not prove process exit; keep observing.
            }
            await Task.Delay(50);
        }
        throw new TimeoutException($"Source node at '{url}' remained reachable after {operation}.");
    }

    private static bool HasConnectionFailure(Exception error)
    {
        for (var current = error; current is not null; current = current.InnerException)
            if (current is HttpRequestException or IOException)
                return true;
        return false;
    }

    public async Task DrainAsync(ZLinkHttpClient client)
    {
        await client.Post("/drain").AsyncRaw();
    }

    public async Task<JoinTargetRes> JoinAsync(
        ZLinkHttpClient client,
        string actorId,
        JoinTargetReq request)
    {
        return (await JoinRawAsync(client, actorId, request)).ToJoinTargetRes();
    }

    public async Task<JoinResponse> JoinRawAsync(
        ZLinkHttpClient client,
        string actorId,
        JoinTargetReq request)
    {
        return (await client.Post($"/actors/{actorId}/join").Body(request)
                   .Async<JoinResponse>()).Body
               ?? throw new InvalidOperationException("Join response was null.");
    }

    public async Task<ProbeRes> ProbeAsync(ZLinkHttpClient client, string actorId, ProbeReq request)
    {
        return (await client.Post($"/actors/{actorId}/probe").Body(request).Async<ProbeRes>()).Body
               ?? throw new InvalidOperationException("Probe response was null.");
    }

    public async Task<NodeActorProbeRes> ProbeFromNodeAsync(
        ZLinkHttpClient client,
        string actorId,
        ProbeReq request,
        TimeSpan? timeout = null)
    {
        // Selecting the HTTP client selects the submitting process. The
        // framework call itself carries only the public global Actor ID.
        return (await client.Post($"/actors/{actorId}/probe-from-node")
                   .Body(new NodeActorCallReq(
                       request.Scenario,
                       request.Marker,
                       checked((int)(timeout ?? TimeSpan.FromSeconds(5)).TotalMilliseconds),
                       request.ReplyMarker))
                   .Async<NodeActorProbeRes>()).Body
               ?? throw new InvalidOperationException(
                   "Node Actor probe response was null.");
    }

    public async Task SendFromNodeAsync(
        ZLinkHttpClient client,
        string actorId,
        HandoffPacket packet)
    {
        // The selected process may have a stale bounded route. No owner RID
        // or ObjectGeneration is supplied by application code.
        await client.Post($"/actors/{actorId}/send-from-node")
            .Body(new NodeActorCallReq(
                packet.Scenario,
                packet.Marker))
            .AsyncRaw();
    }

    public async Task ArmExternalTransportDeliveryAsync(
        string gateId,
        string marker,
        string? afterGateId = null)
    {
        foreach (var admin in TransportProxyAdmins())
        {
            using var response = await _transportProxyClient.PostAsJsonAsync(
                $"{admin}/arm",
                new ExternalTransportGateArm(
                    gateId,
                    marker,
                    afterGateId));
            await EnsureProxySuccessAsync(response, admin, "arm");
        }
    }

    public async Task<ExternalTransportGateRes>
        WaitExternalTransportDeliveryAsync(string gateId)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        do
        {
            var snapshot = await GetExternalTransportDeliveryAsync(gateId);
            if (snapshot.CapturedCount == 1)
                return snapshot;
            ZlinkStreamAssert.Ensure(
                snapshot.CapturedCount == 0,
                $"External transport gate '{gateId}' captured "
                + $"{snapshot.CapturedCount} deliveries, expected one.");
            await Task.Delay(25);
        } while (DateTimeOffset.UtcNow < deadline);

        throw new TimeoutException(
            $"External transport gate '{gateId}' did not capture a delivery.");
    }

    public async Task<ExternalTransportGateRes>
        ReleaseExternalTransportDeliveryAsync(
            string gateId,
            bool requireForwarded = true)
    {
        foreach (var admin in TransportProxyAdmins())
        {
            using var response = await _transportProxyClient.PostAsync(
                $"{admin}/release?gateId={Uri.EscapeDataString(gateId)}",
                null);
            await EnsureProxySuccessAsync(response, admin, "release");
        }

        if (!requireForwarded)
            return await GetExternalTransportDeliveryAsync(gateId);

        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        do
        {
            var snapshot = await GetExternalTransportDeliveryAsync(gateId);
            if (snapshot.ReleasedCount == 1)
                return snapshot;
            await Task.Delay(25);
        } while (DateTimeOffset.UtcNow < deadline);
        return await GetExternalTransportDeliveryAsync(gateId);
    }

    public async Task<ExternalTransportGateRes>
        GetExternalTransportDeliveryAsync(string gateId)
    {
        var snapshots = new List<ExternalTransportGateRes>();
        foreach (var admin in TransportProxyAdmins())
        {
            snapshots.Add(await _transportProxyClient.GetFromJsonAsync<
                    ExternalTransportGateRes>(
                    $"{admin}/snapshot?gateId={Uri.EscapeDataString(gateId)}")
                ?? throw new InvalidOperationException(
                    "External transport gate snapshot was null."));
        }
        return new ExternalTransportGateRes(
            gateId,
            snapshots.Sum(static item => item.CapturedCount),
            snapshots.Sum(static item => item.ReleasedCount),
            snapshots.All(static item => item.Released));
    }

    private IEnumerable<string> TransportProxyAdmins() =>
        Options.TransportProxyAdmins.Distinct(StringComparer.Ordinal);

    private static async Task EnsureProxySuccessAsync(
        HttpResponseMessage response,
        string admin,
        string operation)
    {
        if (response.IsSuccessStatusCode)
            return;
        var detail = await response.Content.ReadAsStringAsync();
        throw new HttpRequestException(
            $"External transport proxy {operation} failed at {admin}: "
            + $"{(int)response.StatusCode} {detail}");
    }

    public async Task<BoundPushRes> BoundPushAsync(
        ZLinkHttpClient client,
        string actorId,
        BoundPushReq request)
    {
        return (await client.Post($"/actors/{actorId}/bound-push").Body(request)
                   .Async<BoundPushRes>()).Body
               ?? throw new InvalidOperationException("Bound push response was null.");
    }

    public async Task<IZlinkStreamConnector> ConnectAndBindAsync(
        string endpoint,
        string scenario,
        ActorRefRes actor)
    {
        var stream = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await stream.Connect.Async();
        var bound = await stream.Request(new BindActorSessionReq(
                scenario, actor.ActorId, actor.NodeRid, actor.Generation))
            .Async<BindActorSessionRes>();
        ZlinkStreamAssert.Ensure(bound.ActorId == actor.ActorId, $"{scenario} session bind actor mismatch.");
        return stream;
    }

    public static async Task<BindActorSessionRes> BindAsync(
        IZlinkStreamConnector stream,
        string scenario,
        ActorRefRes actor)
    {
        var bound = await stream.Request(new BindActorSessionReq(
                scenario,
                actor.ActorId,
                actor.NodeRid,
                actor.Generation))
            .Async<BindActorSessionRes>();
        ZlinkStreamAssert.Ensure(
            bound.ActorId == actor.ActorId
            && bound.NodeRid == actor.NodeRid
            && bound.Generation == actor.Generation,
            $"{scenario} explicit session bind did not preserve the requested ActorRef.");
        return bound;
    }

    public static async Task<SessionBindingsRes> GetSessionBindingsAsync(
        IZlinkStreamConnector stream,
        string scenario) =>
        await stream.Request(new SessionBindingsReq(scenario))
            .Async<SessionBindingsRes>();

    public async Task<IReadOnlyList<ActorEvidence>> WaitEvidenceAsync(
        ZLinkHttpClient client,
        string[] containsAll,
        int timeoutMilliseconds = 10000)
    {
        var evidence = (await client.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                    containsAll,
                    timeoutMilliseconds))
                .Async<IReadOnlyList<ActorEvidence>>()).Body
            ?? throw new InvalidOperationException("Evidence response was null.");
        foreach (var expected in containsAll)
            ZlinkStreamAssert.Ensure(
                evidence.Any(item => EvidenceText(item).Contains(expected, StringComparison.Ordinal)),
                $"Expected evidence marker was not observed: {expected}");
        return evidence;
    }

    public async Task WaitRuntimeEvidenceAsync(ZLinkHttpClient client, params string[] containsAll)
    {
        await WaitRuntimeEvidenceAsync(client, 10000, containsAll);
    }

    public async Task WaitRuntimeEvidenceAsync(
        ZLinkHttpClient client,
        int timeoutMilliseconds,
        params string[] containsAll)
    {
        var evidence = (await client.Post("/runtime-evidence/wait")
                .Body(new EvidenceWaitReq(containsAll, timeoutMilliseconds))
                .Async<string[]>()).Body
            ?? throw new InvalidOperationException("Runtime evidence response was null.");
        foreach (var expected in containsAll)
            ZlinkStreamAssert.Ensure(evidence.Any(item => item.Contains(expected, StringComparison.Ordinal)),
                $"Expected runtime evidence marker was not observed: {expected}");
    }

    public async Task<IReadOnlyList<RelocationInterruptionEvidence>>
        WaitRelocationInterruptionAsync(
            ZLinkHttpClient client,
            string unitKind,
            int minimumCount,
            int timeoutMilliseconds = 10000,
            string? executionMode = null) =>
        (await client.Post("/relocation-interruption/wait")
                .Body(new RelocationInterruptionWaitReq(
                    unitKind,
                    executionMode,
                    minimumCount,
                    timeoutMilliseconds))
                .Timeout(TimeSpan.FromMilliseconds(timeoutMilliseconds + 1000))
                .Async<RelocationInterruptionEvidence[]>())
            .Body
        ?? throw new InvalidOperationException(
            "Relocation interruption evidence response was null.");

    public async Task AssertEvidenceOrderAsync(
        ZLinkHttpClient client,
        string actorId,
        string kind,
        string[] values)
    {
        await WaitEvidenceAsync(client, values.Select(value => $"{actorId}|{kind}|{value}").ToArray());
        var evidence = (await GetEvidenceAsync(client))
            .Where(item => item.ActorId == actorId && item.Kind == kind)
            .Select(item => item.Value)
            .ToArray();
        ZlinkStreamAssert.Ensure(evidence.SequenceEqual(values),
            $"Actor '{actorId}' {kind} order mismatch: {string.Join(",", evidence)}.");
    }

    public async Task<string> TransferForMessageFollowAsync(
        string scenario,
        int stateVersion)
    {
        var actorId = $"actor-message-follow-{scenario}-{Guid.NewGuid():N}";
        var spotId = $"spot-message-follow-{scenario}-{Guid.NewGuid():N}";
        await CreateSpotAsync(NodeB, spotId);
        await CreateActorAsync(NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, stateVersion);
        ZlinkStreamAssert.Ensure((await JoinAsync(NodeA, actorId, new JoinTargetReq(scenario, spotId))).Accepted,
            $"{scenario} transfer was rejected.");
        return actorId;
    }

    public static string EvidenceText(ActorEvidence evidence) =>
        $"{evidence.Scenario}|{evidence.ActorId}|{evidence.Kind}|{evidence.Value}|{evidence.NodeRid}";

    private static ZLinkHttpClient CreateClient(string url) =>
        ZLinkHttpClient.Create(url).Timeout(TimeSpan.FromSeconds(30)).Build();
}

internal sealed record ClientOptions(
    string NodeAUrl,
    int NodeAPid,
    string NodeBUrl,
    string NodeCUrl,
    string NodeDUrl,
    string NodeAStreamEndpoint,
    string NodeBStreamEndpoint,
    string[] TransportProxyAdmins,
    string Scenario)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}

internal sealed record ExternalTransportGateArm(
    string GateId,
    string Marker,
    string? AfterGateId);

internal sealed record ExternalTransportGateRes(
    string GateId,
    int CapturedCount,
    int ReleasedCount,
    bool Released);

internal sealed record JoinResponse(
    string Scenario,
    string ActorId,
    bool Accepted,
    string? SourceNodeRid,
    string? TargetSpotId,
    int? StateVersion,
    string? ErrorKind)
{
    public JoinTargetRes ToJoinTargetRes() => new(
        Scenario, ActorId, Accepted, SourceNodeRid ?? "", TargetSpotId ?? "", StateVersion ?? 0);
}
