using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using MyGame.Shared;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace MyGame.E2E;

internal static class WireChecks
{
    private static bool JsonValuesEqual(JsonNode? left, JsonNode? right)
    {
        if (left is JsonObject leftObject && right is JsonObject rightObject)
            return leftObject.Count == rightObject.Count && leftObject.All(pair =>
                rightObject.TryGetPropertyValue(pair.Key, out var value) && JsonValuesEqual(pair.Value, value));
        if (left is JsonArray leftArray && right is JsonArray rightArray)
            return leftArray.Count == rightArray.Count && leftArray.Zip(rightArray)
                .All(pair => JsonValuesEqual(pair.First, pair.Second));
        if (left?.GetValueKind() == JsonValueKind.Number && right?.GetValueKind() == JsonValueKind.Number)
            return left.GetValue<double>() == right.GetValue<double>();
        return JsonNode.DeepEquals(left, right);
    }

    public static async Task RunAsync()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        var endpoint = $"ws://127.0.0.1:{port}";
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(options => options
            .AddStreamNode("wire-check").Bind(endpoint).AddSession<WireCheckSession>());
        using var host = builder.Build();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await host.StartAsync(deadline.Token);
        try
        {
            await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint), DispatchMode = ZlinkStreamDispatchMode.Immediate
            });
            var samples = new Dictionary<string, string>
            {
                [nameof(QueueRequest)] = "{\"playerId\":\"p1\",\"mode\":\"duel\"}",
                [nameof(MoveRequest)] = "{\"dx\":1,\"dy\":0,\"sequence\":\"7\"}",
                [nameof(ShootRequest)] = "{\"targetId\":\"p2\",\"sequence\":\"9007199254740993\"}",
                [nameof(MoveResult)] = "{\"x\":41,\"y\":10,\"sequence\":\"7\"}",
                [nameof(ShotResult)] = "{\"outcome\":\"hit\",\"targetId\":\"p2\",\"sequence\":\"9007199254740993\"}",
                [nameof(RoomView)] = "{\"roomId\":\"room-x\",\"nodeId\":\"matchA\",\"phase\":\"Playing\",\"winner\":null,\"players\":[{\"playerId\":\"p1\",\"x\":41,\"y\":10,\"hp\":75,\"lastSequence\":\"9007199254740993\"}]}",
                [nameof(ActionRejected)] = "{\"reason\":\"move-budget\",\"retryAfterMs\":120}",
                [nameof(MatchEnded)] = "{\"winner\":\"p1\"}",
            };
            var received = samples.ToDictionary(pair => pair.Key,
                _ => new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously));
            foreach (var pair in received)
                client.On(pair.Key, (message, _) =>
                {
                    pair.Value.TrySetResult(Encoding.UTF8.GetString(message.Payload.Payload.Span));
                    return ValueTask.CompletedTask;
                });
            await client.Connect.Async(deadline.Token);
            await client.Send(new SessionInfoRequest()).Async(deadline.Token);
            foreach (var pair in samples)
            {
                var actual = await received[pair.Key].Task.WaitAsync(deadline.Token);
                if (!JsonValuesEqual(JsonNode.Parse(actual), JsonNode.Parse(pair.Value)))
                    throw new InvalidOperationException($"Wire golden failed: {pair.Key}: {actual}");
            }
            Console.WriteLine($"Wire golden: {samples.Count}/{samples.Count} passed");
        }
        finally { await host.StopAsync(CancellationToken.None); }
    }
}

public sealed class WireCheckSession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context => context;
    public ValueTask OnConnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnDisconnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken ct) =>
        ValueTask.FromException(new InvalidOperationException(error.Message));
    public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload,
        CancellationToken ct)
    {
        await Context.Client.Send(new QueueRequest("p1", "duel")).Async();
        await Context.Client.Send(new MoveRequest(1, 0, 7)).Async();
        await Context.Client.Send(new ShootRequest("p2", 9_007_199_254_740_993L)).Async();
        await Context.Client.Send(new MoveResult(41, 10, 7)).Async();
        await Context.Client.Send(new ShotResult("hit", "p2", 9_007_199_254_740_993L)).Async();
        await Context.Client.Send(new RoomView("room-x", "matchA", "Playing", null,
            [new PlayerView("p1", 41, 10, 75, 9_007_199_254_740_993L)])).Async();
        await Context.Client.Send(new ActionRejected(Rejection.MoveBudget, 120)).Async();
        await Context.Client.Send(new MatchEnded("p1")).Async();
    }
}
