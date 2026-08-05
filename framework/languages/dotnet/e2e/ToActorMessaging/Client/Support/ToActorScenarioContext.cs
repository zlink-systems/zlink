using Systems.Zlink.Stream.Connector.Contracts;
using ToActorMessaging.Shared;
using Zlink.HttpClient;
using Zlink.Framework.Contracts.Actors;

namespace ToActorMessaging.Client.Support;

using Systems.Zlink;
internal sealed class ToActorScenarioContext : IDisposable
{
    private readonly ZLinkHttpClient _actorHttp;
    private readonly ZLinkHttpClient _actorBHttp;
    private readonly ZLinkHttpClient _callerHttp;
    private readonly ZLinkHttpClient _noRouteCallerHttp;
    private readonly ZLinkHttpClient _sessionAHttp;
    private readonly ZLinkHttpClient _sessionBHttp;

    public ToActorScenarioContext(ClientOptions options)
    {
        Options = options;
        _actorHttp = CreateClient(options.ActorUrl);
        _actorBHttp = CreateClient(options.ActorBUrl);
        _callerHttp = CreateClient(options.CallerUrl);
        _noRouteCallerHttp = CreateClient(options.NoRouteCallerUrl);
        _sessionAHttp = CreateClient(options.SessionAUrl);
        _sessionBHttp = CreateClient(options.SessionBUrl);
    }

    public ClientOptions Options { get; }

    public void Dispose()
    {
        _actorHttp.Dispose();
        _actorBHttp.Dispose();
        _callerHttp.Dispose();
        _noRouteCallerHttp.Dispose();
        _sessionAHttp.Dispose();
        _sessionBHttp.Dispose();
    }

    public Task EnsureActorAAsync(string actorId) => PostActorAAsync($"/actors/{actorId}/ensure");

    public async Task EnsureActorBAsync(string actorId)
    {
        await _actorBHttp.Post($"/actors/{actorId}/ensure").Body(new { }).Async<object>();
    }

    public async Task DestroyActorAAsync(string actorId, string scenario)
    {
        await _actorHttp.Post($"/actors/{actorId}/destroy?scenario={scenario}")
            .Body(new { })
            .Async<DestroyActorReply>();
    }

    public async Task DisconnectCallerAsync()
    {
        await _callerHttp.Post("/route/disconnect").Body(new { }).Async<object>();
    }

    public async Task ReconnectCallerAsync()
    {
        await _callerHttp.Post("/route/reconnect").Body(new { }).Async<object>();
    }

    public async Task AssertCallAsync(
        string scenario,
        string actorId,
        string value,
        string expected,
        bool send,
        string? targetNodeRid = null,
        ulong? targetGeneration = null)
    {
        var endpoint = send ? "send" : "request";
        var response = await PostJsonAsync<ActorCallResponse>(
            $"/{endpoint}", new ActorCallRequest(
                scenario,
                actorId,
                value,
                targetNodeRid,
                targetGeneration));
        ZlinkStreamAssert.Ensure(response.Result == expected, $"{scenario} expected '{expected}', got '{response.Result}'.");
        ZlinkStreamAssert.Ensure(response.ErrorKind is null, $"{scenario} unexpected error '{response.ErrorKind}'.");
    }

    //  Config 9 TA-B1: a send has no reply, so its submit result reports local
    //  acceptance only and is not a way to learn whether the remote Actor
    //  exists. The scenario therefore records the outcome and leaves the
    //  verification to the evidence and authority checks that follow.
    public async Task<ActorCallResponse> SendWithoutOutcomeAssertionAsync(
        string scenario,
        string actorId,
        string value,
        string? targetNodeRid = null,
        ulong? targetGeneration = null)
    {
        return await PostJsonAsync<ActorCallResponse>(
            "/send", new ActorCallRequest(
                scenario,
                actorId,
                value,
                targetNodeRid,
                targetGeneration));
    }

    public async Task AssertFailureAsync(
        string scenario,
        string actorId,
        string expectedKind,
        bool send,
        string? targetNodeRid = null,
        ulong? targetGeneration = null)
    {
        var endpoint = send ? "send" : "request";
        var response = await PostJsonAsync<ActorCallResponse>(
            $"/{endpoint}", new ActorCallRequest(
                scenario,
                actorId,
                "missing",
                targetNodeRid,
                targetGeneration));
        ZlinkStreamAssert.Ensure(response.ErrorKind == expectedKind,
            $"{scenario} expected '{expectedKind}', got '{response.ErrorKind}'.");
    }

    public async Task<ActorRef> CaptureAsync(string actorId)
    {
        return (await _callerHttp.Post($"/refs/{actorId}/capture")
            .Body(new { })
            .Async<ActorRef>()).Body;
    }

    public async Task<ActorEvidence[]> GetAllActorEvidenceAsync()
    {
        var actorA = (await _actorHttp.Get("/evidence").Async<ActorEvidence[]>()).Body;
        var actorB = (await _actorBHttp.Get("/evidence").Async<ActorEvidence[]>()).Body;
        return actorA.Concat(actorB).ToArray();
    }

    public async Task AssertRouteAbsentAsync(string actorId)
    {
        var status = await GetRouteStatusAsync(actorId);
        ZlinkStreamAssert.Ensure(!status.Exists, $"Actor route '{actorId}' was created unexpectedly.");
    }

    public async Task WaitForRouteAbsentAsync(string actorId)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        do
        {
            if (!(await GetRouteStatusAsync(actorId)).Exists) return;
            await Task.Delay(100);
        } while (DateTimeOffset.UtcNow < deadline);

        throw new InvalidOperationException($"Actor route '{actorId}' remained after destroy completed.");
    }

    public async Task AssertCachedFailureAsync(string scenario, string actorId, string expectedKind)
    {
        var response = await PostJsonAsync<ActorCallResponse>(
            "/cached/request", new ActorCallRequest(scenario, actorId, "failure"));
        ZlinkStreamAssert.Ensure(response.ErrorKind == expectedKind,
            $"{scenario} expected '{expectedKind}', got '{response.ErrorKind}'.");
    }

    public async Task AssertNoRouteCallerFailureAsync(
        string scenario,
        ActorRef actor)
    {
        await _noRouteCallerHttp.Post("/route/disconnect")
            .Body(new { })
            .Async<object>();
        var response = (await _noRouteCallerHttp.Post("/request")
            .Body(new ActorCallRequest(
                scenario,
                actor.ActorId,
                "no-route",
                actor.NodeRid.ToString(),
                actor.ObjectGeneration))
            .Async<ActorCallResponse>()).Body;
        // 10.0.0 conversion table (spec 05 §13.1): an explicit route
        // disconnect removes the peer from the member snapshot, so Core
        // reports NOT_FOUND and the framework surfaces NotFound.
        // Unavailable is reserved for a known member whose pipe is
        // not ready.
        ZlinkStreamAssert.Ensure(
            response.ErrorKind is "NotFound" or "Unavailable",
            $"{scenario} expected NotFound or Unavailable without a route, got '{response.ErrorKind}'.");
    }

    public async Task AssertNoRouteCallerRecoveryAsync(
        string scenario,
        ActorRef actor,
        string value)
    {
        await _noRouteCallerHttp.Post("/route/reconnect").Body(new { }).Async<object>();
        // Mesh admission after a reconnect is asynchronous (spec 21 §5); the
        // recovery contract is that requests succeed once the peer readmits,
        // so poll within a bounded window instead of racing the handshake.
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        ActorCallResponse response;
        while (true)
        {
            response = (await _noRouteCallerHttp.Post("/request")
                .Body(new ActorCallRequest(
                    scenario,
                    actor.ActorId,
                    value,
                    actor.NodeRid.ToString(),
                    actor.ObjectGeneration))
                .Async<ActorCallResponse>()).Body;
            if (response.ErrorKind is null || DateTimeOffset.UtcNow >= deadline) break;
            await Task.Delay(100);
        }

        ZlinkStreamAssert.Ensure(response.Result == $"reply:{value}" && response.ErrorKind is null,
            $"{scenario} request after route recovery failed: '{response.ErrorKind ?? response.Result}'.");
    }

    public async Task<IZlinkStreamConnector> ConnectAsync(string endpoint)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 128
        });
        try
        {
            await connector.Connect.Async();
            return connector;
        }
        catch
        {
            await connector.DisposeAsync();
            throw;
        }
    }

    public async Task<IZlinkStreamConnector> ConnectAndBindAsync(string endpoint, string actorId)
    {
        var connector = await ConnectAsync(endpoint);
        try
        {
            var reply = await connector.Request(new BindActorRequest(actorId))
                .PacketName("BindActorRequest")
                .Async<BindActorReply>();
            ZlinkStreamAssert.Ensure(reply.ActorId == actorId, $"Actor bind reply mismatch for '{actorId}'.");
            var probe = await connector.Request(new ActorAsk("bind-probe", actorId, "bound"))
                .PacketName("ActorAsk")
                .Async<ActorReply>();
            ZlinkStreamAssert.Ensure(probe.ActorId == actorId, $"Actor bind probe mismatch for '{actorId}'.");
            return connector;
        }
        catch
        {
            await connector.DisposeAsync();
            throw;
        }
    }

    public async Task AssertBoundPushAsync(
        IZlinkStreamConnector bound,
        IZlinkStreamConnector? unbound,
        string scenario,
        string actorId,
        string value)
    {
        var received = bound.WaitFor<BoundPushNotify>().Async().AsTask();
        var reply = (await _actorHttp.Post($"/actors/{actorId}/push")
            .Body(new BoundPushRequest(scenario, actorId, value))
            .Async<BoundPushReply>()).Body;
        var notify = await received;
        ZlinkStreamAssert.Ensure(reply.Submitted, $"{scenario} bound push was not submitted.");
        ZlinkStreamAssert.Ensure(notify.Payload == new BoundPushNotify(scenario, actorId, value),
            $"{scenario} bound push payload mismatch.");
        if (unbound is null) return;
        await unbound.ExpectNone<BoundPushNotify>()
            .Within(TimeSpan.FromMilliseconds(300))
            .Async();
    }

    public async Task AssertBoundPushFailureAsync(
        bool actorB,
        string scenario,
        string actorId,
        string value)
    {
        var actor = actorB ? _actorBHttp : _actorHttp;
        // A disconnect reaches the Actor's owner node as a relayed frame, so
        // the session side reporting the binding gone does not mean the owner
        // has seen it yet. Poll until it has.
        BoundPushReply reply;
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (true)
        {
            reply = (await actor.Post($"/actors/{actorId}/push")
                .Body(new BoundPushRequest(scenario, actorId, value))
                .Async<BoundPushReply>()).Body;
            if (!reply.Submitted || DateTime.UtcNow >= deadline) break;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        // Spec 20 §360: a push with no current binding is session-not-bound,
        // which surfaces as InvalidOperation. The Actor is not missing - the
        // binding has to exist first, and the same call succeeds once it does.
        ZlinkStreamAssert.Ensure(!reply.Submitted && reply.ErrorKind == "InvalidOperation",
            $"{scenario} expected InvalidOperation with no bound session, got '{reply.ErrorKind}'.");
    }

    public async Task<BoundSessionSnapshot[]> GetBoundSessionSnapshotsAsync(string actorId)
    {
        var sessionA = (await _sessionAHttp.Get($"/bindings/{actorId}")
            .Async<BoundSessionSnapshot>()).Body;
        var sessionB = (await _sessionBHttp.Get($"/bindings/{actorId}")
            .Async<BoundSessionSnapshot>()).Body;
        return [sessionA, sessionB];
    }

    public async Task<BoundSessionSnapshot[]> WaitForSessionUnboundAsync(string actorId)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        BoundSessionSnapshot[] snapshots;
        do
        {
            snapshots = await GetBoundSessionSnapshotsAsync(actorId);
            if (snapshots.All(snapshot => snapshot.SessionRid is null)) return snapshots;
            await Task.Delay(100);
        } while (DateTimeOffset.UtcNow < deadline);

        throw new InvalidOperationException(
            $"Actor '{actorId}' still had a live bound-session snapshot after disconnect.");
    }

    public async Task AssertNoActorEvidenceAsync(string actorId)
    {
        var entries = (await _actorHttp.Get("/evidence").Async<ActorEvidence[]>()).Body;
        ZlinkStreamAssert.Ensure(entries.All(item => item.ActorId != actorId),
            $"Missing actor '{actorId}' unexpectedly produced handler or lifecycle evidence.");
    }

    private async Task PostActorAAsync(string path)
    {
        await _actorHttp.Post(path).Body(new { }).Async<object>();
    }

    private async Task<T> PostJsonAsync<T>(string path, object body)
    {
        return (await _callerHttp.Post(path).Body(body).Async<T>()).Body
               ?? throw new InvalidOperationException($"Endpoint '{path}' returned null.");
    }

    private async Task<ActorRouteStatus> GetRouteStatusAsync(string actorId) =>
        (await _callerHttp.Get($"/directory/{actorId}")
            .Async<ActorRouteStatus>()).Body;

    private static ZLinkHttpClient CreateClient(string url) =>
        ZLinkHttpClient.Create(url).Timeout(TimeSpan.FromSeconds(30)).Build();
}
