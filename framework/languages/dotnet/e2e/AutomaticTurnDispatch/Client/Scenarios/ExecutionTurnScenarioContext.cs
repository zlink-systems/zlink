// Hides deployment addressing and connector mechanics shared by execution-turn scenarios.
using AutomaticTurnDispatch.Shared;
using Zlink.HttpClient;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal sealed class ExecutionTurnScenarioContext(
    IZlinkStreamConnector client,
    ZLinkHttpClient? playA = null,
    ZLinkHttpClient? playB = null)
{
    private readonly ZLinkHttpClient? _playA = playA;
    private readonly ZLinkHttpClient? _playB = playB;
    private string? _spotRid;
    private readonly Dictionary<string, string> _evidenceNodes =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _spotNodes =
        new(StringComparer.Ordinal);

    internal async Task<string> SpotAsync()
    {
        if (_spotRid is not null) return _spotRid;
        _spotRid = $"execution-turn-{Guid.NewGuid():N}";
        await EnsureSpotAsync(_spotRid, "play-a");
        return _spotRid;
    }

    internal Task<AwaitActorScenarioContext> ActorsAsync() =>
        ActorsAsync(AutomaticTurnDispatchNames.SpotType);

    internal async Task<AwaitActorScenarioContext> ActorsAsync(string spotType)
    {
        var spot = await SpotAsync();
        if (!string.Equals(spotType, AutomaticTurnDispatchNames.SpotType, StringComparison.Ordinal))
        {
            spot = $"execution-turn-{spotType}-{Guid.NewGuid():N}";
            await EnsureSpotAsync(spot, "play-a", spotType);
        }
        var actorA = $"actor-a-{Guid.NewGuid():N}";
        var actorB = $"actor-b-{Guid.NewGuid():N}";
        var result = await client.Request(new BindAwaitActorsReq(spot, [actorA, actorB], spotType))
            .Timeout(TimeSpan.FromSeconds(30))
            .Async<BindAwaitActorsRes>();
        ZlinkStreamAssert.Ensure(result.Actors.Length == 2, "Execution turn actor binding failed.");
        return new AwaitActorScenarioContext(spot, actorA, actorB);
    }

    internal async Task EnsureActorInSpotAsync(
        string actorId,
        string spotRid,
        string scenarioId,
        string spotType = AutomaticTurnDispatchNames.SpotType)
    {
        var requestId = NewId(scenarioId);
        var reply = await ActorRequest(actorId,
                new ActorJoinAwaitReq(requestId, spotRid))
            .Async<ActorAwaitRes>();
        ZlinkStreamAssert.Ensure(reply.Marker.Contains("actor-join", StringComparison.Ordinal),
            $"{scenarioId} actor placement was not deferred.");
        var completionMarker = reply.Marker switch
        {
            "actor-join-await-released" => "actor-join-await",
            "actor-join-deferred" => "actor-join",
            _ => throw new InvalidOperationException(
                $"{scenarioId} returned unknown Actor Join marker '{reply.Marker}'.")
        };
        await AssertJoinCompletionAsync(
            requestId,
            completionMarker,
            spotRid,
            scenarioId);
        await WaitActorDispatchReadyAsync(actorId, scenarioId, spotType);
    }

    internal Task EnsureSpotAsync(string spotRid, string targetNode) =>
        EnsureSpotAsync(spotRid, targetNode, AutomaticTurnDispatchNames.SpotType);

    internal async Task SetExclusivePlacementAsync(string nodeRid)
    {
        if (_playA is null || _playB is null)
            throw new InvalidOperationException(
                "AutomaticTurnDispatch placement control requires Play HTTP endpoints.");

        ZlinkStreamAssert.Ensure(
            nodeRid is "play-a" or "play-b",
            $"Unknown placement node '{nodeRid}'.");
        if (nodeRid == "play-a")
        {
            await SetPlacementWeightAsync(_playB, 0);
            await SetPlacementWeightAsync(_playA, 100);
        }
        else
        {
            await SetPlacementWeightAsync(_playA, 0);
            await SetPlacementWeightAsync(_playB, 100);
        }
    }

    internal async Task RestoreDefaultPlacementAsync()
    {
        if (_playA is null || _playB is null)
            throw new InvalidOperationException(
                "AutomaticTurnDispatch placement control requires Play HTTP endpoints.");

        await SetPlacementWeightAsync(_playA, 100, verifyLocal: false);
        await SetPlacementWeightAsync(_playB, 100, verifyLocal: false);
    }

    private static async Task SetPlacementWeightAsync(
        ZLinkHttpClient node,
        int weight,
        bool verifyLocal = true)
    {
        var response = (await node.Post("/placement-weight")
                .Body(new PlacementWeightReq(weight, verifyLocal))
                .Async<PlacementWeightRes>())
            .Body;
        ZlinkStreamAssert.Ensure(
            response.Weight == weight,
            $"Placement weight expected {weight}, got {response.Weight}.");
    }

    internal async Task<string> EnsureSpotNodeAsync(
        string spotRid,
        string targetNode,
        string spotType = AutomaticTurnDispatchNames.SpotType)
    {
        await EnsureSpotAsync(spotRid, targetNode, spotType);
        return _spotNodes[spotRid];
    }

    internal async Task EnsureSpotAsync(string spotRid, string targetNode, string spotType)
    {
        var request = new EnsureSpotReq(spotRid, spotType);
        var builder = client.Request(request);
        if (targetNode != "play-a")
            builder.Metadata(AutomaticTurnDispatchNames.TargetNodeRidMetadata, targetNode);
        var result = await builder.Timeout(TimeSpan.FromSeconds(30)).Async<EnsureSpotRes>();
        ZlinkStreamAssert.Ensure(result.SpotRid == spotRid, $"Spot creation failed for {spotRid}.");
        _spotNodes[spotRid] = ControlNode(result.NodeRid);
    }

    internal ValueTask SendSpotAsync<T>(T message, string spotRid)
    {
        return client.Send(message)
            .Metadata(AutomaticTurnDispatchNames.SpotRidMetadata, spotRid)
            .Async();
    }

    internal ZlinkStreamTypedRequestBuilder SpotRequest<TRequest>(
        string spotRid,
        TRequest request,
        TimeSpan? timeout = null)
    {
        return client.Request(request)
            .Metadata(AutomaticTurnDispatchNames.SpotRidMetadata, spotRid)
            .Timeout(timeout ?? TimeSpan.FromSeconds(30));
    }

    internal ZlinkStreamTypedRequestBuilder ActorRequest<TRequest>(string actorId, TRequest request)
    {
        return client.Request(request)
            .Metadata(AutomaticTurnDispatchNames.ActorIdMetadata, actorId)
            .Timeout(TimeSpan.FromSeconds(30));
    }

    internal async Task WaitActorDispatchReadyAsync(
        string actorId,
        string scenarioId,
        string spotType = AutomaticTurnDispatchNames.SpotType)
    {
        var deadline = DateTimeOffset.UtcNow.AddSeconds(10);
        Systems.Zlink.Stream.Connector.Contracts.ZlinkStreamException? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var requestId = NewId($"{scenarioId}-route-ready");
                if (string.Equals(
                        spotType,
                        AutomaticTurnDispatchNames.PerActorSpotType,
                        StringComparison.Ordinal))
                {
                    _ = await ActorRequest(
                            actorId,
                            new PerActorFastReq(requestId, "route-ready"))
                        .Async<ActorAwaitRes>();
                }
                else
                {
                    _ = await ActorRequest(
                            actorId,
                            new ActorFastReq(requestId, "route-ready"))
                        .Async<ActorAwaitRes>();
                }
                return;
            }
            catch (Systems.Zlink.Stream.Connector.Contracts.ZlinkStreamException error)
                when (error.Error.Code
                      == Systems.Zlink.Stream.Connector.Contracts
                          .ZlinkStreamErrorCode.RemoteError)
            {
                last = error;
                await Task.Delay(25);
            }
        }

        throw new TimeoutException(
            $"{scenarioId} Actor '{actorId}' session route did not become ready.",
            last);
    }

    internal async Task<string[]> EvidenceAsync(
        string requestId,
        string marker,
        string? targetNode = null,
        int minimumCount = 1)
    {
        if (targetNode is not null)
            return await EvidenceFromNodeAsync(
                requestId,
                marker,
                targetNode,
                minimumCount,
                CancellationToken.None);
        if (_evidenceNodes.TryGetValue(requestId, out var evidenceNode))
            return await EvidenceFromNodeAsync(
                requestId,
                marker,
                evidenceNode,
                minimumCount,
                CancellationToken.None);

        // User Spot placement does not expose or accept a physical node RID.
        // Poll both nodes without cancelling an in-flight connector request.
        var deadline = DateTimeOffset.UtcNow.AddSeconds(30);
        while (DateTimeOffset.UtcNow < deadline)
        {
            foreach (var node in new[] { "play-a", "play-b" })
            {
                var response = await client.Request(
                        new AwaitEvidenceReq(requestId))
                    .Metadata(
                        AutomaticTurnDispatchNames.TargetNodeRidMetadata,
                        node)
                    .Timeout(TimeSpan.FromSeconds(3))
                    .Async<AwaitEvidenceRes>();
                if (response.Evidence.Count(line =>
                        line.Contains($"request={requestId}", StringComparison.Ordinal)
                        && line.Contains(marker, StringComparison.Ordinal))
                    >= minimumCount)
                {
                    _evidenceNodes[requestId] = node;
                    return response.Evidence;
                }
            }
            await Task.Delay(25);
        }
        throw new TimeoutException(
            $"No evidence node completed marker '{marker}'.");
    }

    private async Task<string[]> EvidenceFromNodeAsync(
        string requestId,
        string marker,
        string targetNode,
        int minimumCount,
        CancellationToken cancellationToken) =>
        (await client.Request(new AwaitEvidenceWaitReq(
                requestId,
                marker,
                MinimumCount: minimumCount))
            .Metadata(
                AutomaticTurnDispatchNames.TargetNodeRidMetadata,
                ControlNode(targetNode))
            .Timeout(TimeSpan.FromSeconds(30))
            .Async<AwaitEvidenceRes>(cancellationToken)).Evidence;

    private static string ControlNode(string objectNodeRid)
    {
        const int generatedSuffixLength = 37;
        return objectNodeRid.Length > generatedSuffixLength
               && objectNodeRid[^generatedSuffixLength] == '-'
               && Guid.TryParse(
                   objectNodeRid.AsSpan(
                       objectNodeRid.Length + 1 - generatedSuffixLength),
                   out _)
            ? objectNodeRid[..^generatedSuffixLength]
            : objectNodeRid;
    }

    internal async Task AssertJoinCompletionAsync(
        string requestId,
        string marker,
        string targetSpotId,
        string scenarioId)
    {
        _spotNodes.TryGetValue(targetSpotId, out var targetNode);
        var evidence = await EvidenceAsync(
            requestId,
            $"{marker}-completed",
            targetNode);
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains($"request={requestId}", StringComparison.Ordinal)
                && line.Contains($"{marker}-completed", StringComparison.Ordinal)
                && line.Contains($"spot={targetSpotId}", StringComparison.Ordinal)
                && line.Contains("accepted=True", StringComparison.OrdinalIgnoreCase)),
            $"{scenarioId} deferred Actor Join did not complete at Spot '{targetSpotId}'.");
    }

    internal static string NewId(string scenarioId) => $"{scenarioId}-{Guid.NewGuid():N}";

}
