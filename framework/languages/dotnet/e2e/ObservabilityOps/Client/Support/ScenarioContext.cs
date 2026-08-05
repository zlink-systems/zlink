using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;
using System.Text.RegularExpressions;

namespace ObservabilityOps.Client.Support;

internal sealed class ScenarioContext(ClientOptions options) : IDisposable
{
    private readonly Dictionary<string, string> _playNodeIds =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _workflowNodeIds =
        new(StringComparer.Ordinal);
    public ClientOptions Options { get; } = options;
    public ZLinkHttpClient PlayA { get; } = ZLinkHttpClient.Create(options.PlayAUrl).Build();
    public ZLinkHttpClient PlayB { get; } = ZLinkHttpClient.Create(options.PlayBUrl).Build();
    public ZLinkHttpClient PlayC { get; } = ZLinkHttpClient.Create(options.PlayCUrl).Build();
    public ZLinkHttpClient PlayD { get; } = ZLinkHttpClient.Create(options.PlayDUrl).Build();
    public ZLinkHttpClient Session { get; } = ZLinkHttpClient.Create(options.SessionUrl).Build();
    public ZLinkHttpClient WorkflowA { get; } = ZLinkHttpClient.Create(options.WorkflowAUrl).Build();
    public ZLinkHttpClient WorkflowB { get; } = ZLinkHttpClient.Create(options.WorkflowBUrl).Build();

    public async Task<IZlinkStreamConnector> ConnectAsync(
        string? endpoint = null,
        bool persistentReconnect = false,
        bool reconnectEnabled = true,
        Action<IZlinkStreamConnector>? configure = null)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint ?? Options.SessionEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = !reconnectEnabled
                ? new ZlinkStreamReconnectOptions { Enabled = false }
                : persistentReconnect
                ? new ZlinkStreamReconnectOptions
                {
                    Enabled = true,
                    InitialDelay = TimeSpan.FromMilliseconds(100),
                    MaxDelay = TimeSpan.FromMilliseconds(300),
                    BackoffFactor = 2,
                    MaxAttempts = null
                }
                : new ZlinkStreamReconnectOptions { Enabled = true },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 256
        });
        configure?.Invoke(connector);
        await connector.Connect.Async();
        return connector;
    }

    // Placement decides which play node owns a room. Scenarios need that
    // node's harness role label, both to ask the right host for evidence and
    // because RequireSharedFlow matches labels against flow file names, where
    // a routing id never appears.
    public async Task<string> PlayRoleForNodeAsync(string nodeRid) =>
        string.Equals(nodeRid, await PlayNodeIdAsync("play-a"), StringComparison.Ordinal)
            ? "play-a"
            : "play-b";

    public async Task<string> WorkflowRoleForNodeAsync(string nodeRid) =>
        string.Equals(nodeRid, await WorkflowNodeIdAsync("workflow-a"), StringComparison.Ordinal)
            ? "workflow-a"
            : "workflow-b";

    public async Task<string[]> WaitPlayEvidenceForNodeAsync(
        string nodeRid,
        params string[] markers) =>
        await PlayRoleForNodeAsync(nodeRid) == "play-a"
            ? await WaitPlayAEvidenceAsync(markers)
            : await WaitPlayBEvidenceAsync(markers);

    public async Task<string[]> WaitPlayAEvidenceAsync(params string[] markers)
    {
        return (await PlayA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(markers, []))
            .Async<string[]>()).Body;
    }

    public async Task<string[]> WaitPlayBEvidenceAsync(params string[] markers)
    {
        return (await PlayB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(markers, []))
            .Async<string[]>()).Body;
    }

    /// <summary>
    /// Retries a request that raced a session rebind. The runtime types that
    /// failure RetryAfterBackoff, so it is a transient the caller resends.
    /// </summary>
    public static async Task<T> RequestWithRebindRetryAsync<T>(
        Func<Task<T>> request)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return await request();
            }
            catch (Exception error) when (attempt < 10 && IsBindingRebind(error))
            {
                await Task.Delay(100);
            }
        }
    }

    private static bool IsBindingRebind(Exception error) =>
        error.ToString().Contains("session binding", StringComparison.Ordinal);

    public static bool IsRemoteFrameworkError(
        Exception error,
        params string[] frameworkErrorKinds)
    {
        ArgumentNullException.ThrowIfNull(error);
        ArgumentNullException.ThrowIfNull(frameworkErrorKinds);

        return error is ZlinkStreamException
            {
                Error.Code: ZlinkStreamErrorCode.RemoteError,
                Error.Message: { } message
            }
            && frameworkErrorKinds.Any(kind =>
                !string.IsNullOrWhiteSpace(kind)
                && message.StartsWith(kind + ":", StringComparison.Ordinal));
    }

    public static bool IsTransientTrafficFailure(Exception error)
    {
        ArgumentNullException.ThrowIfNull(error);

        return error is ZlinkStreamException
            {
                Error.Code: ZlinkStreamErrorCode.RequestTimeout
            }
            || IsRemoteFrameworkError(
                error,
                "Unavailable",
                "DeadlineExceeded",
                "ShuttingDown");
    }

    public async Task<ActorJoinCompletedNotify> JoinRoomAsync(
        IZlinkStreamConnector connector,
        string actorId,
        string roomId)
    {
        var completion = connector.WaitFor<ActorJoinCompletedNotify>()
            .Where(message => message.Payload.ActorId == actorId
                              && message.Payload.SpotId == roomId)
            .Timeout(TimeSpan.FromSeconds(15))
            .Async()
            .AsTask();
        // A join that relocates the Actor can land while its session binding is
        // being rebound. The runtime marks that RetryAfterBackoff, so it is a
        // transient the caller retries rather than a failure.
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                await connector.Request(new JoinRoomReq(roomId)).Async<JoinRoomRes>();
                break;
            }
            catch (Exception error) when (attempt < 10 && IsBindingRebind(error))
            {
                await Task.Delay(100);
            }
        }

        var result = (await completion).Payload;
        if (!result.Accepted)
            throw new InvalidOperationException(
                $"Actor '{actorId}' could not join room '{roomId}': {result.Error}.");
        return result;
    }

    public async Task<ActorJoinCompletedNotify> ReturnToEntrySpotAsync(
        IZlinkStreamConnector connector,
        string actorId,
        string marker,
        DateTimeOffset? retryUntil = null)
    {
        var retryDeadline = retryUntil
                            ?? DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        var completion = connector.WaitFor<ActorJoinCompletedNotify>()
            .Where(message => message.Payload.ActorId == actorId
                              && message.Payload.SpotId is null
                              && message.Payload.Marker == marker)
            .Timeout(TimeSpan.FromSeconds(15))
            .Async()
            .AsTask();
        await connector.Request(new ReturnToLobbyReq(marker))
            .Async<ReturnToLobbyRes>();
        var result = (await completion).Payload;
        if (!result.Accepted)
        {
            //  런타임은 일시 상태를 재시도 가능한 kind로 알려준다. 완료 통지가
            //  retriable 플래그를 나르지 않으므로 kind 이름으로 판별해 예산 안에서
            //  다시 시도한다. 정책 거절(`Rejected`)은 그대로 종단 실패다.
            if (result.Error is "Unavailable" or "DeadlineExceeded"
                && DateTimeOffset.UtcNow < retryDeadline)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(250));
                return await ReturnToEntrySpotAsync(
                    connector, actorId, marker, retryDeadline);
            }

            throw new InvalidOperationException(
                $"Actor '{actorId}' could not return to its Entry Spot: {result.Error}.");
        }

        return result;
    }

    public async Task<CreateRoomRes> CreateRoomOnObservedNodeAsync(
        string nodeRid,
        string idPrefix,
        string mode = "normal")
    {
        nodeRid = await ResolvePlayNodeIdAsync(nodeRid);
        for (var attempt = 0; attempt < 64; attempt++)
        {
            var roomId = $"{idPrefix}-{attempt}-{Guid.NewGuid():N}";
            var created = (await PlayA.Post("/rooms")
                .Body(new CreateRoomReq(roomId, mode))
                .Async<CreateRoomRes>()).Body;
            if (created.NodeRid == nodeRid) return created;
            await PlayA.Post($"/rooms/{created.RoomRid}/close").AsyncRaw();
        }

        throw new InvalidOperationException(
            $"Framework placement did not select observed node '{nodeRid}' "
            + "within the bounded setup attempts.");
    }

    public async Task SetPlayPlacementWeightAsync(string role, int weight)
    {
        var client = role switch
        {
            "play-a" => PlayA,
            "play-b" => PlayB,
            "play-c" => PlayC,
            "play-d" => PlayD,
            _ => throw new ArgumentOutOfRangeException(nameof(role))
        };
        var response = (await client.Post("/placement-weight")
            .Body(new PlacementWeightReq(weight))
            .Async<PlacementWeightRes>()).Body;
        if (response.Weight != weight)
            throw new InvalidOperationException(
                $"Placement weight for '{role}' was not applied: "
                + $"{response.Weight} != {weight}.");
    }

    public async Task<ActivateInstanceSpotRes> ActivateInstanceOnObservedNodeAsync(
        string nodeRid,
        string idPrefix)
    {
        nodeRid = await ResolvePlayNodeIdAsync(nodeRid);
        for (var attempt = 0; attempt < 64; attempt++)
        {
            var spotId = $"{idPrefix}-{attempt}-{Guid.NewGuid():N}";
            var activated = (await PlayA.Post($"/instances/{spotId}")
                .Body(new ActivateInstanceSpotReq("active"))
                .Async<ActivateInstanceSpotRes>()).Body;
            if (activated.NodeRid == nodeRid) return activated;
            await PlayA.Post($"/spots/{activated.SpotId}/close").AsyncRaw();
        }

        throw new InvalidOperationException(
            $"Framework Instance Spot placement did not select observed node "
            + $"'{nodeRid}' within the bounded setup attempts.");
    }

    public async Task<PlayWorkload> PreparePlayWorkloadAsync(
        string sourceNode,
        string scenario)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var room = await CreateRoomOnObservedNodeAsync(
            sourceNode, $"room-{scenario}-{suffix}");
        var instance = await ActivateInstanceOnObservedNodeAsync(
            sourceNode, $"instance-{scenario}-{suffix}");
        var actorId = $"actor-{scenario}-{suffix}";
        var connector = await ConnectAsync();
        try
        {
            await connector.Request(new AuthenticateReq(actorId))
                .Async<AuthenticateRes>();
            var joined = await JoinRoomAsync(
                connector, actorId, room.RoomRid);
            if (joined.NodeRid != sourceNode)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' did not join source '{sourceNode}'.");

            var evidence = await WaitForPlayWorkloadAsync(
                room.RoomRid, actorId, sourceNode, TimeSpan.FromSeconds(10));
            return new PlayWorkload(
                actorId,
                room,
                instance,
                evidence.ActorRows.Single(row => row.ActorId == actorId)
                    .Generation,
                evidence.SpotRows.Single(row =>
                    row.SpotRid == room.RoomRid).Generation,
                connector);
        }
        catch
        {
            await connector.DisposeAsync();
            throw;
        }
    }

    public async Task<string> PlayNodeIdAsync(string role)
    {
        if (_playNodeIds.TryGetValue(role, out var cached))
            return cached;
        var client = role switch
        {
            "play-a" => PlayA,
            "play-b" => PlayB,
            "play-c" => PlayC,
            "play-d" => PlayD,
            _ => throw new ArgumentOutOfRangeException(nameof(role))
        };
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var identity = (await client.Get("/identity")
                    .Async<NodeIdentityRes>()).Body;
                _playNodeIds[role] = identity.NodeRid;
                return identity.NodeRid;
            }
            catch
            {
                await Task.Delay(100);
            }
        }
        throw new TimeoutException(
            $"Runtime NodeRid for role '{role}' did not become observable.");
    }

    private Task<string> ResolvePlayNodeIdAsync(string nodeRid) =>
        nodeRid is "play-a" or "play-b"
            ? PlayNodeIdAsync(nodeRid)
            : Task.FromResult(nodeRid);

    public async Task<string> WorkflowNodeIdAsync(string role)
    {
        if (_workflowNodeIds.TryGetValue(role, out var cached))
            return cached;
        var client = role switch
        {
            "workflow-a" => WorkflowA,
            "workflow-b" => WorkflowB,
            _ => throw new ArgumentOutOfRangeException(nameof(role))
        };
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var identity = (await client.Get("/identity")
                    .Async<NodeIdentityRes>()).Body;
                _workflowNodeIds[role] = identity.NodeRid;
                return identity.NodeRid;
            }
            catch
            {
                await Task.Delay(100);
            }
        }
        throw new TimeoutException(
            $"Runtime NodeRid for role '{role}' did not become observable.");
    }

    public async Task<EvidenceSnapshot> WaitForPlayWorkloadAsync(
        string roomId,
        string actorId,
        string ownerNode,
        TimeSpan timeout)
    {
        ownerNode = await ResolvePlayNodeIdAsync(ownerNode);
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = (await PlayA.Get("/evidence")
                .Query("spotRid", roomId)
                .Query("actorId", actorId)
                .Async<EvidenceSnapshot>()).Body;
            if (snapshot.SpotRows.Any(row =>
                    row.SpotRid == roomId && row.NodeRid == ownerNode)
                && snapshot.ActorRows.Any(row =>
                    row.ActorId == actorId && row.NodeRid == ownerNode))
                return snapshot;
            await Task.Delay(100);
        }

        throw new TimeoutException(
            $"Workload did not converge on '{ownerNode}'.");
    }

    public ZLinkHttpClient Play(string nodeRid) =>
        _playNodeIds.FirstOrDefault(pair => pair.Value == nodeRid).Key switch
        {
            "play-a" => PlayA,
            "play-b" => PlayB,
            _ => throw new InvalidOperationException(
                $"Unknown runtime Play NodeRid '{nodeRid}'.")
        };

    public ZLinkHttpClient OtherPlay(string nodeRid) =>
        _playNodeIds.FirstOrDefault(pair => pair.Value == nodeRid).Key switch
        {
            "play-a" => PlayB,
            "play-b" => PlayA,
            _ => throw new InvalidOperationException(
                $"Unknown runtime Play NodeRid '{nodeRid}'.")
        };

    public ZLinkHttpClient Workflow(string nodeRid) =>
        _workflowNodeIds.FirstOrDefault(
            pair => pair.Value == nodeRid).Key switch
        {
            "workflow-a" => WorkflowA,
            "workflow-b" => WorkflowB,
            _ => throw new InvalidOperationException(
                $"Unknown runtime Workflow NodeRid '{nodeRid}'.")
        };

    public ZLinkHttpClient OtherWorkflow(string nodeRid) =>
        _workflowNodeIds.FirstOrDefault(
            pair => pair.Value == nodeRid).Key switch
        {
            "workflow-a" => WorkflowB,
            "workflow-b" => WorkflowA,
            _ => throw new InvalidOperationException(
                $"Unknown runtime Workflow NodeRid '{nodeRid}'.")
        };

    public string OtherPlayNode(string nodeRid) =>
        _playNodeIds.FirstOrDefault(pair => pair.Value == nodeRid).Key switch
        {
            "play-a" => _playNodeIds["play-b"],
            "play-b" => _playNodeIds["play-a"],
            _ => throw new InvalidOperationException(
                $"Unknown runtime Play NodeRid '{nodeRid}'.")
        };

    public string OtherWorkflowNode(string nodeRid) =>
        _workflowNodeIds.FirstOrDefault(
            pair => pair.Value == nodeRid).Key switch
        {
            "workflow-a" => _workflowNodeIds["workflow-b"],
            "workflow-b" => _workflowNodeIds["workflow-a"],
            _ => throw new InvalidOperationException(
                $"Unknown runtime Workflow NodeRid '{nodeRid}'.")
        };

    public string RequireSharedFlow(string packetName, params string[] roleLabels)
    {
        var matching = Directory.GetFiles(Options.LogDir, "flow-*.log")
            .SelectMany(path => File.ReadLines(path).Select(line => (Path: path, Line: line)))
            .Where(item => item.Line.Contains($"packet={packetName}", StringComparison.Ordinal))
            .ToArray();
        foreach (var flow in matching.Select(item => FlowId(item.Line)).Where(id => id is not null).Distinct())
        {
            if (roleLabels.All(label => matching.Any(item =>
                    Path.GetFileName(item.Path).Contains(label, StringComparison.Ordinal)
                    && string.Equals(FlowId(item.Line), flow, StringComparison.Ordinal))))
                return flow!;
        }
        throw new InvalidOperationException(
            $"No shared flow for packet '{packetName}' crossed roles: {string.Join(", ", roleLabels)}.");
    }

    public string[] ReadFlowLines(string roleLabel) => Directory.GetFiles(Options.LogDir, "flow-*.log")
        .Where(path => Path.GetFileName(path).Contains(roleLabel, StringComparison.Ordinal))
        .SelectMany(File.ReadLines).ToArray();

    public static async Task<MaintenanceStatus> WaitForRelocationAsync(
        ZLinkHttpClient client,
        TimeSpan timeout)
    {
        return (await client.Post("/relocate/wait")
            .Query("timeoutMs", ((int)timeout.TotalMilliseconds).ToString())
            .Timeout(timeout + TimeSpan.FromSeconds(2))
            .Async<MaintenanceStatus>()).Body;
    }

    public static async Task<MaintenanceStatus> WaitForShutdownAsync(
        ZLinkHttpClient client,
        TimeSpan timeout)
    {
        return (await client.Post("/shutdown/wait")
            .Query("timeoutMs", ((int)timeout.TotalMilliseconds).ToString())
            .Timeout(timeout + TimeSpan.FromSeconds(2))
            .Async<MaintenanceStatus>()).Body;
    }

    private static string? FlowId(string line) =>
        Regex.Match(line, "(?:^| )flow=([^ ]+)", RegexOptions.CultureInvariant) is { Success: true } match
            ? match.Groups[1].Value
            : null;

    public void Dispose()
    {
        PlayA.Dispose();
        PlayB.Dispose();
        PlayC.Dispose();
        PlayD.Dispose();
        Session.Dispose();
        WorkflowA.Dispose();
        WorkflowB.Dispose();
    }
}

internal sealed record PlayWorkload(
    string ActorId,
    CreateRoomRes Room,
    ActivateInstanceSpotRes Instance,
    long ActorGeneration,
    long RoomGeneration,
    IZlinkStreamConnector Connector) : IAsyncDisposable
{
    public ValueTask DisposeAsync() => Connector.DisposeAsync();
}
