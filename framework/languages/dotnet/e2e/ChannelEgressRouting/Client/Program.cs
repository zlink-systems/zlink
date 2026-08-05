using System.Diagnostics;
using System.Text.Json;
using ChannelEgressRouting.Client;
using ChannelEgressRouting.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Configuration;
using Zlink.HttpClient;

var options = ClientOptions.Parse(args);
var http = options.Urls.ToDictionary(
    static entry => entry.Key,
    static entry => ZLinkHttpClient.Create(entry.Value)
        .Timeout(TimeSpan.FromSeconds(5))
        .Build(),
    StringComparer.Ordinal);
string[] knownScenarios =
[
    "CH-E2E-01", "CH-E2E-02", "CH-E2E-03", "CH-E2E-05",
    "CH-E2E-04A", "CH-E2E-04B", "CH-E2E-04C", "CH-E2E-06",
    "CH-E2E-07A", "CH-E2E-07B", "CH-E2E-07C", "CH-E2E-08",
    "CH-E2E-09", "CH-E2E-10", "CH-E2E-11", "CH-E2E-12",
    "CH-REG-01", "CH-REG-02", "CH-REG-03", "CH-REG-04", "CH-REG-05",
    "CH-REG-06", "CH-REG-07", "CH-REG-08", "CH-REG-09", "CH-REG-10",
];
var selected = options.Scenario == "all"
    ? knownScenarios
    : options.Scenario.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
foreach (var scenario in selected)
{
    if (!knownScenarios.Contains(scenario, StringComparer.Ordinal))
        throw new InvalidOperationException($"Unknown ChannelEgressRouting selector '{scenario}'.");
    await RunAsync(scenario);
    Console.WriteLine($"scenario {scenario} passed");
}

Console.WriteLine("channel-egress-routing client result=passed");
foreach (var client in http.Values)
    client.Dispose();

async Task RunAsync(string scenario)
{
    switch (scenario)
    {
        case "CH-E2E-01":
        case "CH-REG-01":
            await AssertRequestAsync("session", ChannelEgressNames.Play, "one-session-play", "play");
            await AssertRequestAsync("play", ChannelEgressNames.Session, "one-play-session", "session");
            break;
        case "CH-E2E-02":
            var cascade = await InvokeRequestAsync(
                "session", ChannelEgressNames.Play, "cascade", "cascade");
            Require(cascade.Succeeded && cascade.Reply?.Role == "play",
                "cascade request did not complete at Play.");
            var cascadeReply = cascade.Reply
                ?? throw new InvalidOperationException("cascade reply is missing.");
            Require(cascadeReply.Downstream.SequenceEqual(
                    new[]
                    {
                        $"audit:{ChannelEgressNames.Audit}",
                        $"workflow-100:{ChannelEgressNames.Workflow}"
                    })
                    || cascadeReply.Downstream.SequenceEqual(
                    new[]
                    {
                        $"audit:{ChannelEgressNames.Audit}",
                        $"workflow-300:{ChannelEgressNames.Workflow}"
                    }),
                "cascade selected the wrong downstream egress.");
            break;
        case "CH-E2E-03":
            await AssertSpotWorkflowAndTimerAsync();
            break;
        case "CH-E2E-04A":
            await AssertWeightedWorkflowAsync("workflow-client", 160);
            await SetClientServerWeightAsync("workflow300", 0);
            await WaitClientServerTargetCountAsync("workflow-client", 1);
            await AssertOnlyWorkflowServerAsync("workflow-client", "workflow-100", 32);
            await SetClientServerWeightAsync("workflow300", 300);
            await WaitClientServerTargetCountAsync("workflow-client", 2);
            break;
        case "CH-E2E-04B":
            await AssertDrainingWorkflowAsync();
            break;
        case "CH-E2E-04C":
            await AssertRoleReplacementAsync();
            break;
        case "CH-E2E-05":
            await AssertServerOnlyProcessCannotRequestAsync();
            break;
        case "CH-E2E-06":
            await AssertInvalidStartupAsync("route-clientserver-conflict");
            await AssertInvalidStartupAsync("duplicate-workflow-client");
            break;
        case "CH-E2E-07A":
            var missing = await InvokeRequestAsync(
                "session", "not.registered", "missing");
            Require(!missing.Succeeded && missing.Error == "NotFound",
                $"missing channel should be NotFound, got {missing.Error}.");
            Require(missing.ElapsedMilliseconds < 1000,
                "missing channel result was not immediate.");
            break;
        case "CH-E2E-07B":
            await AssertRequestAsync("session", ChannelEgressNames.Api, "ready-api", "api");
            break;
        case "CH-E2E-07C":
            var unavailable = await InvokeRequestAsync(
                "session", ChannelEgressNames.Api, "unavailable-api");
            Require(!unavailable.Succeeded && unavailable.Error == "Unavailable",
                $"known but unavailable channel should be Unavailable, got {unavailable.Error}.");
            await AssertNoEvidenceAsync("api", "request|", "id=unavailable-api");
            break;
        case "CH-E2E-08":
            await AssertClientServerStateAddressAsync();
            break;
        case "CH-E2E-10":
            await AssertSendAsync(
                "workflow-client", ChannelEgressNames.Workflow, "one-way");
            await WaitEvidenceAsync(
                new[] { "workflow100", "workflow300" },
                "send|",
                "id=one-way");
            break;
        case "CH-E2E-09":
            await AssertAutomaticEndpointsAsync();
            break;
        case "CH-E2E-11":
            await AssertRequestAsync("session", ChannelEgressNames.Api, "remote-api", "api");
            await AssertSendAsync("session", ChannelEgressNames.Api, "remote-api-send");
            await WaitEvidenceAsync(
                new[] { "api" },
                "send|role=api",
                "id=remote-api-send");
            break;
        case "CH-E2E-12":
        case "CH-REG-10":
            await AssertWeightedWorkflowAsync("workflow100", 100);
            await SetClientServerWeightAsync("workflow100", 0);
            await WaitClientServerTargetCountAsync("workflow100", 1);
            await AssertOnlyWorkflowServerAsync("workflow100", "workflow-300", 32);
            await SetClientServerWeightAsync("workflow100", 100);
            await WaitClientServerTargetCountAsync("workflow100", 2);
            break;
        case "CH-REG-04":
            await AssertTerminalOnceAsync();
            break;
        case "CH-REG-02":
            await AssertStateAddressRegressionAsync();
            break;
        case "CH-REG-05":
            await AssertRoleReplacementAsync();
            break;
        case "CH-REG-03":
            await AssertClassicFanoutAsync();
            break;
        case "CH-REG-06":
            var local = await InvokeRequestAsync(
                "session", ChannelEgressNames.Play, "latency");
            Require(local.Succeeded && local.ElapsedMilliseconds < 1000,
                $"local request exceeded the baseline: {local.ElapsedMilliseconds}ms.");
            break;
        case "CH-REG-07":
            AssertSampleTopologyFixture();
            break;
        case "CH-REG-08":
            await AssertSinglePhysicalPeerAsync("session", ChannelEgressNames.GameMesh);
            await AssertSinglePhysicalPeerAsync("play", ChannelEgressNames.GameMesh);
            break;
        case "CH-REG-09":
            AssertSampleSourceDoesNotHideRoutingInput();
            break;
        default:
            throw new InvalidOperationException(
                $"{scenario} is not executable in the current Config 12 lane.");
    }
}

async Task AssertRoleReplacementAsync()
{
    using var beforeDocument = JsonDocument.Parse(
        await File.ReadAllTextAsync(
            Path.Combine(options.LogDir, "workflow.before-replacement.json")));
    using var afterDocument = JsonDocument.Parse(
        await File.ReadAllTextAsync(
            Path.Combine(options.LogDir, "workflow.after-replacement.json")));
    var before = beforeDocument.RootElement.GetProperty("targets")
        .EnumerateArray()
        .Select(target => target.GetProperty("rid").GetString() ?? string.Empty)
        .ToHashSet(StringComparer.Ordinal);
    var after = afterDocument.RootElement.GetProperty("targets")
        .EnumerateArray()
        .Select(target => target.GetProperty("rid").GetString() ?? string.Empty)
        .ToHashSet(StringComparer.Ordinal);
    Require(before.Count == 2 && after.Count == 2,
        "Role replacement did not preserve two Ready targets.");
    Require(before.Intersect(after).Count() == 1
            && before.Except(after).Count() == 1
            && after.Except(before).Count() == 1,
        "Automatic replacement did not replace exactly one runtime RID.");
    var request = await InvokeRequestAsync(
        "workflow-client",
        ChannelEgressNames.Workflow,
        "replacement-request");
    Require(request.Succeeded
            && request.Reply?.Role is "workflow-100" or "workflow-300-new",
        "Request did not complete after automatic role replacement.");
}

async Task AssertDrainingWorkflowAsync()
{
    var shutdown = await http["workflow300"].Post("/shutdown").AsyncRaw();
    Require(shutdown.Status is >= 200 and < 300,
        $"draining workflow server rejected shutdown: {shutdown.Status}.");
    await WaitClientServerTargetCountAsync("workflow-client", 1);
    await AssertOnlyWorkflowServerAsync("workflow-client", "workflow-100", 32);
}

async Task AssertSpotWorkflowAndTimerAsync()
{
    var actor = await CreateActorOnAsync("play", "10-play", "spot-workflow");
    var reply = (await http["play"]
            .Post($"/objects/actors/{actor.ActorId}/workflow")
            .Body(new ChannelSpotWorkflowRequest("spot-workflow"))
            .Async<ChannelSpotWorkflowReply>())
        .Body;
    Require(reply.StateVersion == 2, "Spot state was not changed after the awaited reply.");
    Require(reply.WorkflowRole is "workflow-100" or "workflow-300" or "workflow-300-new",
        $"Spot workflow selected an unexpected server: {reply.WorkflowRole}.");
    await WaitEvidenceAsync(
        ["play"],
        "spot-workflow|phase=resumed",
        "id=spot-workflow",
        "version=2");
    await WaitEvidenceAsync(
        ["play"],
        "spot-timer|phase=resumed");
}

async Task AssertClientServerStateAddressAsync()
{
    var actor = await CreateActorOnAsync("session", null, "state-address");
    var spot = await CreateSpotOnAsync("play", null, "state-address-room");
    var reply = (await http["workflow100"]
            .Post("/objects/state-address")
            .Body(new ChannelObjectScenarioRequest(
                actor.ActorId,
                spot.SpotId,
                "state-address"))
            .Async<ChannelProbeReply>())
        .Body;
    Require(reply.Downstream.Length == 2
            && reply.Downstream[0].StartsWith($"spot:{spot.SpotId}:", StringComparison.Ordinal)
            && reply.Downstream[1].StartsWith($"actor:{actor.ActorId}:", StringComparison.Ordinal),
        "ClientServer handler did not preserve the Spot then Actor call order.");
}

async Task AssertStateAddressRegressionAsync()
{
    var actor = await CreateActorOnAsync("session", "00-session", "snapshot-join");
    var spot = await CreateSpotOnAsync("play", "10-play", "snapshot-room");

    var before = await ProbeActorAsync("workflow100", actor.ActorId, "before-join");
    Require(before.ActorId == actor.ActorId && before.StateVersion == 1,
        "Actor direct request failed before Snapshot join.");

    var node = (await http["workflow100"]
            .Post($"/objects/nodes/{Uri.EscapeDataString(actor.NodeRid)}/probe")
            .Body(new ChannelObjectProbeRequest("node-direct"))
            .Async<ChannelProbeReply>())
        .Body;
    Require(node.Role == "session" && node.Channel == ChannelEgressNames.GameMesh,
        "Node direct request selected the wrong MeshNode.");

    var spotProbe = await ProbeSpotAsync("workflow100", spot.SpotId, "spot-direct");
    Require(spotProbe.SpotId == spot.SpotId,
        "Spot direct request selected a different Spot.");

    var join = (await http["session"]
            .Post($"/objects/actors/{actor.ActorId}/join")
            .Body(new ChannelActorJoinRequest("snapshot-join", spot.SpotId))
            .Async<ChannelActorJoinReply>())
        .Body;
    Require(join.Submitted, "Snapshot Actor join was not submitted.");

    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
    ChannelObjectProbeReply? after = null;
    do
    {
        try
        {
            after = await ProbeActorAsync(
                "workflow100",
                actor.ActorId,
                "after-join");
            if (after.SpotId == spot.SpotId)
                break;
        }
        catch (HttpRequestException)
        {
        }
        await Task.Delay(100);
    } while (DateTimeOffset.UtcNow < deadline);
    Require(after?.SpotId == spot.SpotId,
        "Actor did not become addressable at the target Spot.");
    Require(after!.StateVersion == before.StateVersion,
        "Snapshot Actor state changed during join.");

    await using var connector = ZlinkStreamConnectorFactory.Create(
        new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(options.StreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 32
        });
    await connector.Connect.Async();
    var bound = await connector
        .Request(new ChannelBindActorRequest(actor.ActorId))
        .PacketName(nameof(ChannelBindActorRequest))
        .Async<ChannelBindActorReply>();
    Require(bound.ActorId == actor.ActorId,
        "Session binding selected a different Actor.");
    var notificationTask = connector
        .WaitFor<ChannelBoundPushNotification>()
        .Async()
        .AsTask();
    var push = (await http["play"]
            .Post($"/objects/actors/{actor.ActorId}/bound-push")
            .Body(new ChannelBoundPushRequest("after-snapshot-join"))
            .Async<ChannelBoundPushReply>())
        .Body;
    var notification = await notificationTask.WaitAsync(TimeSpan.FromSeconds(5));
    Require(push.Submitted
            && notification.Payload.ActorId == actor.ActorId
            && notification.Payload.SpotId == spot.SpotId
            && notification.Payload.StateVersion == before.StateVersion,
        "Bound-session push did not follow the relocated Actor.");
}

async Task<ChannelActorCreateReply> CreateActorOnAsync(
    string source,
    string? nodeRidPrefix,
    string idPrefix)
{
    for (var attempt = 0; attempt < 32; attempt++)
    {
        var actorId = $"{idPrefix}-{Guid.NewGuid():N}";
        var actor = (await http[source]
                .Post("/objects/actors")
                .Body(new ChannelActorCreateRequest(actorId))
                .Async<ChannelActorCreateReply>())
            .Body;
        if (nodeRidPrefix is null
            || actor.NodeRid.StartsWith(nodeRidPrefix, StringComparison.Ordinal))
            return actor;
    }
    throw new InvalidOperationException(
        $"Could not place an Actor on node prefix '{nodeRidPrefix}'.");
}

async Task<ChannelSpotCreateReply> CreateSpotOnAsync(
    string source,
    string? nodeRidPrefix,
    string idPrefix)
{
    for (var attempt = 0; attempt < 32; attempt++)
    {
        var spotId = $"{idPrefix}-{Guid.NewGuid():N}";
        var spot = (await http[source]
                .Post("/objects/spots")
                .Body(new ChannelSpotCreateRequest(spotId))
                .Async<ChannelSpotCreateReply>())
            .Body;
        if (nodeRidPrefix is null
            || spot.NodeRid.StartsWith(nodeRidPrefix, StringComparison.Ordinal))
            return spot;
    }
    throw new InvalidOperationException(
        $"Could not place a Spot on node prefix '{nodeRidPrefix}'.");
}

async Task<ChannelObjectProbeReply> ProbeActorAsync(
    string source,
    string actorId,
    string id)
{
    return (await http[source]
            .Post($"/objects/actors/{actorId}/probe")
            .Body(new ChannelObjectProbeRequest(id))
            .Async<ChannelObjectProbeReply>())
        .Body;
}

async Task<ChannelObjectProbeReply> ProbeSpotAsync(
    string source,
    string spotId,
    string id)
{
    return (await http[source]
            .Post($"/objects/spots/{spotId}/probe")
            .Body(new ChannelObjectProbeRequest(id))
            .Async<ChannelObjectProbeReply>())
        .Body;
}

async Task<RouteInvokeResult> InvokeRequestAsync(
    string role,
    string channel,
    string id,
    string mode = "echo")
{
    return (await http[role]
            .Post("/request")
            .Body(new RouteInvokeRequest(channel, id, mode))
            .Async<RouteInvokeResult>())
        .Body;
}

async Task AssertRequestAsync(
    string source,
    string channel,
    string id,
    string expectedRole)
{
    var result = await InvokeRequestAsync(source, channel, id);
    Require(result.Succeeded, $"request failed: {result.Error}");
    Require(result.Reply?.Role == expectedRole,
        $"channel {channel} selected {result.Reply?.Role}, expected {expectedRole}.");
    var reply = result.Reply
                ?? throw new InvalidOperationException("route reply is missing.");
    Require(reply.Channel == channel,
        $"handler observed channel {reply.Channel}, expected {channel}.");
}

async Task AssertSendAsync(string source, string channel, string id)
{
    var result = (await http[source]
            .Post("/send")
            .Body(new RouteInvokeRequest(channel, id))
            .Async<SendInvokeResult>())
        .Body;
    Require(result.Succeeded, $"send failed: {result.Error}");
}

async Task AssertWeightedWorkflowAsync(string source, int count)
{
    var before100 = EvidenceCount("workflow100", "request|");
    var before300 = EvidenceCount("workflow300", "request|");
    for (var index = 0; index < count; index++)
    {
        var result = await InvokeRequestAsync(
            source,
            ChannelEgressNames.Workflow,
            $"weighted-{source}-{index}");
        Require(result.Succeeded && result.Reply is not null,
            $"workflow request failed: {result.Error}");
        var reply = result.Reply
                    ?? throw new InvalidOperationException("workflow reply is missing.");
        Require(reply.Role is "workflow-100" or "workflow-300",
            $"unexpected workflow server {reply.Role}.");
    }

    var count100 = EvidenceCount("workflow100", "request|") - before100;
    var count300 = EvidenceCount("workflow300", "request|") - before300;
    Require(count100 + count300 == count,
        $"workflow handler count mismatch: {count100}+{count300}!={count}.");
    var ratio = count300 / (double)Math.Max(1, count100);
    Require(ratio is >= 2.0 and <= 4.5,
        $"workflow weight ratio did not approach 1:3: {count100}:{count300}.");
}

async Task AssertOnlyWorkflowServerAsync(
    string source,
    string expectedRole,
    int count)
{
    for (var index = 0; index < count; index++)
    {
        var result = await InvokeRequestAsync(
            source,
            ChannelEgressNames.Workflow,
            $"single-weight-{source}-{index}");
        Require(result.Succeeded && result.Reply?.Role == expectedRole,
            $"weight-zero exclusion selected {result.Reply?.Role ?? result.Error}.");
    }
}

async Task SetClientServerWeightAsync(string role, int weight)
{
    await http[role]
        .Post($"/client-server/{ChannelEgressNames.Workflow}/weight/{weight}")
        .Async<JsonElement>();
}

async Task WaitClientServerTargetCountAsync(string role, int expected)
{
    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
    do
    {
        var status = (await http[role]
                .Get($"/client-server/{ChannelEgressNames.Workflow}")
                .Async<JsonElement>())
            .Body;
        if (status.GetProperty("readyTargetCount").GetInt32() == expected)
            return;
        await Task.Delay(50);
    } while (DateTimeOffset.UtcNow < deadline);

    throw new InvalidOperationException(
        $"{role} did not converge to {expected} ready ClientServer target(s).");
}

async Task AssertServerOnlyProcessCannotRequestAsync()
{
    var serverOnly = await InvokeRequestAsync(
        "workflow-server-only",
        ChannelEgressNames.Workflow,
        "server-only-request");
    Require(!serverOnly.Succeeded && serverOnly.Error == "NotFound",
        $"Server-only process unexpectedly started a ClientServer request: {serverOnly.Error}.");

    var normal = await InvokeRequestAsync(
        "workflow-client",
        ChannelEgressNames.Workflow,
        "server-only-normal-request");
    Require(normal.Succeeded
            && normal.Reply?.Role is "workflow-100" or "workflow-300",
        $"normal Client role request failed: {normal.Error}.");
    await WaitEvidenceAsync(
        ["workflow100", "workflow300"],
        "request|",
        "id=server-only-normal-request");
}

async Task AssertInvalidStartupAsync(string mode)
{
    var config = Path.Combine(options.ConfigDir, $"invalid-{mode}.json");
    var json = JsonSerializer.Serialize(new
    {
        Options = new
        {
            Role = $"invalid-{mode}",
            Rid = $"invalid-{mode}",
            HttpUrl = "http://127.0.0.1:0",
            RedisEndpoint = options.RedisEndpoint,
            RedisKeyPrefix = options.RedisKeyPrefix,
            EvidenceFile = Path.Combine(options.LogDir, $"invalid-{mode}.evidence.log"),
            RouteServers = new[]
                { mode == "route-clientserver-conflict" ? "workflow.command" : "game.invalid" },
            RouteClients = Array.Empty<string>(),
            WorkflowClient = true,
            WorkflowServer = false,
            WorkflowWeight = 100,
            InvalidMode = mode
        }
    });
    await File.WriteAllTextAsync(config, json);

    using var process = Process.Start(new ProcessStartInfo
    {
        FileName = "dotnet",
        ArgumentList =
        {
            "run", "--no-build", "--project", options.InvalidServerProject,
            "--", "--config", config
        },
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false
    }) ?? throw new InvalidOperationException("Failed to start invalid role.");
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    await process.WaitForExitAsync(timeout.Token);
    var output = await process.StandardOutput.ReadToEndAsync()
                 + await process.StandardError.ReadToEndAsync();
    Require(process.ExitCode != 0, $"{mode} unexpectedly started.");
    Require(output.Contains("ChannelName", StringComparison.OrdinalIgnoreCase)
            || output.Contains("already registered", StringComparison.OrdinalIgnoreCase),
        $"{mode} did not report both registration causes.");
}

async Task AssertTerminalOnceAsync()
{
    var calls = Enumerable.Range(0, 32)
        .Select(index => InvokeRequestAsync(
            "session", ChannelEgressNames.Play, $"terminal-{index}"))
        .ToArray();
    var results = await Task.WhenAll(calls);
    Require(results.All(static result => result.Succeeded),
        "a terminal race request failed.");
    Require(results.Select(static result => result.Reply!.Id).Distinct().Count() == results.Length,
        "a request completed more than once or used another correlation.");
}

async Task AssertClassicFanoutAsync()
{
    var spot = await CreateSpotOnAsync(
        "play",
        "10-play",
        "logical-multicast-room");
    var logicalId = $"logical-{Guid.NewGuid():N}";
    var logicalBefore = EvidenceCount(
        "play",
        "logical-multicast|",
        $"spot={spot.SpotId}",
        $"id={logicalId}");
    await http["session"].Post($"/logical/{logicalId}").AsyncRaw();
    var logicalDeadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
    do
    {
        if (EvidenceCount(
                "play",
                "logical-multicast|",
                $"spot={spot.SpotId}",
                $"id={logicalId}") - logicalBefore == 1)
            break;
        await Task.Delay(50);
    } while (DateTimeOffset.UtcNow < logicalDeadline);
    Require(EvidenceCount(
                "play",
                "logical-multicast|",
                $"spot={spot.SpotId}",
                $"id={logicalId}") - logicalBefore == 1,
        "Logical Multicast did not reach the subscribed remote Spot exactly once.");

    var before = EvidenceCount("play", "fanout|", "id=fanout-regression");
    await http["audit"].Post("/fanout/fanout-regression").AsyncRaw();
    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
    do
    {
        if (EvidenceCount("play", "fanout|", "id=fanout-regression") - before == 1)
            return;
        await Task.Delay(50);
    } while (DateTimeOffset.UtcNow < deadline);
    throw new InvalidOperationException("classic fanout did not reach Play exactly once.");
}

async Task AssertAutomaticEndpointsAsync()
{
    var rows = (await http["session"]
            .Get("/locations")
            .Async<JsonElement[]>())
        .Body;
    Require(rows.Length >= 5, $"expected topology descriptors, got {rows.Length}.");
    var meshNames = rows
        .Select(row => row.GetProperty("meshName").GetString())
        .Where(static value => value is not null)
        .ToHashSet(StringComparer.Ordinal);
    Require(meshNames.Contains(ChannelEgressNames.GameMesh),
        "location descriptor is missing for the game RouteMesh.");
    foreach (var row in rows)
    {
        var endpoint = row.GetProperty("endpoint").GetString()
                       ?? throw new InvalidOperationException("advertised endpoint is missing.");
        Require(endpoint.StartsWith("tcp://127.0.0.1:", StringComparison.Ordinal),
            $"endpoint did not apply AdvertiseHost: {endpoint}.");
        Require(!endpoint.EndsWith(":0", StringComparison.Ordinal),
            $"automatic port was not replaced by the actual bound port: {endpoint}.");
    }

    await AssertRequestAsync(
        "session",
        ChannelEgressNames.Api,
        "automatic-route-mesh",
        "api");
    var clientServer = await InvokeRequestAsync(
        "workflow-client",
        ChannelEgressNames.Workflow,
        "automatic-client-server");
    Require(clientServer.Succeeded
            && clientServer.Reply?.Role.StartsWith("workflow-", StringComparison.Ordinal) == true,
        "automatic ClientServer endpoint did not complete at a remote server.");

    await http["audit"].Post("/fanout/automatic-fanout").AsyncRaw();
    await WaitEvidenceAsync(
        ["play"],
        "fanout|role=play",
        "id=automatic-fanout");

    await using var connector = ZlinkStreamConnectorFactory.Create(
        new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(options.StreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 32
        });
    await connector.Connect.Async();
}

async Task AssertSinglePhysicalPeerAsync(string role, string mesh)
{
    var response = (await http[role]
            .Get($"/topology/{mesh}")
            .Async<JsonElement>())
        .Body;
    Require(response.GetProperty("readyPeerCount").GetInt32() > 0,
        $"{role} has no ready {mesh} peer.");
    var peers = response.GetProperty("peers")
        .EnumerateArray()
        .Select(peer => peer.GetProperty("rid").GetString())
        .ToArray();
    Require(peers.Length == peers.Distinct(StringComparer.Ordinal).Count(),
        $"{role} exposes duplicate physical peer identities.");

    var rows = (await http["session"]
            .Get("/locations")
            .Async<JsonElement[]>())
        .Body
        .Where(row => string.Equals(
            row.GetProperty("meshName").GetString(),
            mesh,
            StringComparison.Ordinal))
        .ToArray();
    Require(rows.Select(row => row.GetProperty("rid").GetString())
            .Distinct(StringComparer.Ordinal)
            .Count() == rows.Length,
        $"location topology contains duplicate {mesh} listener identities.");
    Require(rows.Select(row => row.GetProperty("endpoint").GetString())
            .Distinct(StringComparer.Ordinal)
            .Count() == rows.Length,
        $"location topology contains duplicate {mesh} listener endpoints.");
}

void AssertSampleSourceDoesNotHideRoutingInput()
{
    var root = FindRepositoryRoot(AppContext.BaseDirectory);
    var sampleRoot = Path.Combine(root, "framework", "languages", "dotnet", "samples");
    foreach (var file in Directory.EnumerateFiles(sampleRoot, "*.cs", SearchOption.AllDirectories))
    {
        var source = File.ReadAllText(file);
        Require(!source.Contains("PreferredNodeRid", StringComparison.Ordinal)
                && !source.Contains("PreferredRoutingId", StringComparison.Ordinal),
            $"sample hides placement routing in {file}.");
    }
}

void AssertSampleTopologyFixture()
{
    var root = FindRepositoryRoot(AppContext.BaseDirectory);
    var fixturePath = Path.Combine(
        root,
        "framework", "doc", "framework", "common", "sample",
        "fixtures", "channel-topology.json");
    using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));
    var samples = fixture.RootElement.GetProperty("samples");
    Require(samples.EnumerateObject().Count() == 7,
        "common sample topology fixture must contain exactly seven samples.");

    var sampleRoot = Path.Combine(root, "framework", "languages", "dotnet", "samples");
    foreach (var sample in samples.EnumerateObject())
    {
        var sampleDirectory = Path.Combine(sampleRoot, sample.Name);
        Require(Directory.Exists(sampleDirectory),
            $"sample source directory is missing: {sample.Name}.");
        var source = string.Join(
            "\n",
            Directory.EnumerateFiles(sampleDirectory, "*.cs", SearchOption.AllDirectories)
                .Select(File.ReadAllText));
        foreach (var mesh in sample.Value.GetProperty("routeMeshes").EnumerateArray())
        {
            var name = mesh.GetString()
                       ?? throw new InvalidOperationException("route mesh name is missing.");
            Require(source.Contains(name, StringComparison.Ordinal),
                $"{sample.Name} source does not expose fixture RouteMesh '{name}'.");
        }
        foreach (var channel in sample.Value.GetProperty("channels").EnumerateObject())
            Require(source.Contains(channel.Name, StringComparison.Ordinal),
                $"{sample.Name} source does not expose fixture ChannelName '{channel.Name}'.");
    }
}

async Task WaitEvidenceAsync(string[] roles, params string[] fragments)
{
    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
    do
    {
        if (roles.Sum(role => EvidenceCount(role, fragments)) == 1)
            return;
        await Task.Delay(50);
    } while (DateTimeOffset.UtcNow < deadline);
    throw new InvalidOperationException(
        $"Expected one evidence entry containing {string.Join(',', fragments)}.");
}

async Task AssertNoEvidenceAsync(string role, params string[] fragments)
{
    var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(1);
    do
    {
        Require(EvidenceCount(role, fragments) == 0,
            $"Unexpected evidence entry containing {string.Join(',', fragments)}.");
        await Task.Delay(50);
    } while (DateTimeOffset.UtcNow < deadline);
}

int EvidenceCount(string role, params string[] fragments)
{
    if (!File.Exists(options.EvidenceFiles[role])) return 0;
    return File.ReadLines(options.EvidenceFiles[role])
        .Count(line => fragments.All(fragment =>
            line.Contains(fragment, StringComparison.Ordinal)));
}

static string FindRepositoryRoot(string start)
{
    var directory = new DirectoryInfo(start);
    while (directory is not null)
    {
        if (File.Exists(Path.Combine(directory.FullName, "AGENTS.md")))
            return directory.FullName;
        directory = directory.Parent;
    }
    throw new InvalidOperationException("Repository root not found.");
}

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
