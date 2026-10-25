using System.Net;
using System.Net.Sockets;
using System.Text;
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
        if (left?.GetValueKind() == System.Text.Json.JsonValueKind.Number &&
            right?.GetValueKind() == System.Text.Json.JsonValueKind.Number)
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
                ["MoveRequest"] = "{\"delta\":{\"x\":1,\"y\":0,\"z\":-2},\"sequence\":\"7\"}",
                ["ShotResult"] = "{\"outcome\":\"hit\",\"targetId\":null,\"sequence\":\"9007199254740991\"}",
                ["RoomView"] = "{\"roomId\":\"room-x\",\"nodeId\":\"matchA\",\"players\":[{\"playerId\":\"a\",\"position\":{\"x\":48,\"y\":0,\"z\":18},\"hp\":100,\"lastSequence\":\"4\"}],\"phase\":\"Playing\",\"winner\":null}",
                ["ActionRejected"] = "{\"reason\":\"move-budget\",\"retryAfterMs\":120}",
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
            Console.WriteLine("Wire golden: 1/1 passed (4 samples)");
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
        await Context.Client.Send(new MoveRequest(new(1, 0, -2), 7)).Async();
        await Context.Client.Send(new ShotResult(ShotOutcome.Hit, null, GameNames.MaxSequence)).Async();
        await Context.Client.Send(new RoomView("room-x", "matchA",
            [new("a", new(48, 0, 18), 100, 4)], MatchPhase.Playing, null)).Async();
        await Context.Client.Send(new ActionRejected(Rejection.MoveBudget, 120)).Async();
    }
}
