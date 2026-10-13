using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class ConnectorJsonProfileTests
{
    [Theory]
    [InlineData(long.MinValue)]
    [InlineData(long.MaxValue)]
    public async Task Typed_request_reaches_server_decode_and_reads_server_reply(long tick)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
            options
                .AddStreamNode("profile.stream")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<ProfileSession>()
        );
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var state = await host
                .Services.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetStartedStateForRoutingAsync(CancellationToken.None);
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(state.StreamNodes.Values.Single().BoundEndpoint!),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new() { Enabled = false },
                    Heartbeat = new() { Enabled = false },
                }
            );
            await connector.Connect.Async();
            var reply = await connector
                .Request(
                    new Counters
                    {
                        Tick = tick,
                        Count = ulong.MaxValue,
                        Small = int.MaxValue,
                    }
                )
                .PacketName("counters")
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<Counters>();
            Assert.Equal(tick, reply.Tick);
            Assert.Equal(ulong.MaxValue, reply.Count);
            Assert.Equal(int.MaxValue, reply.Small);
            await connector.Close.Async();
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Theory]
    [InlineData(long.MinValue)]
    [InlineData(long.MaxValue)]
    public void Default_connector_and_server_preserve_integer_boundaries(long tick)
    {
        var expected = new Counters
        {
            Tick = tick,
            Count = ulong.MaxValue,
            Small = int.MaxValue,
        };
        var encoded = expected.ToJson();
        using var document = JsonDocument.Parse(encoded.Payload);
        Assert.Equal(JsonValueKind.String, document.RootElement.GetProperty("tick").ValueKind);
        Assert.Equal(JsonValueKind.String, document.RootElement.GetProperty("count").ValueKind);
        Assert.Equal(JsonValueKind.Number, document.RootElement.GetProperty("small").ValueKind);
        var received = ZLinkFrameworkJsonPayloadCodec.Deserialize<Counters>(encoded.Payload.Span)!;
        Assert.Equal(tick, received.Tick);
        Assert.Equal(ulong.MaxValue, received.Count);
        var reply = new ZlinkStreamEncodedPayload(
            ZlinkStreamCodec.Json,
            ZLinkFrameworkJsonPayloadCodec.Serialize(expected)
        );
        var decoded = reply.FromJson<Counters>();
        Assert.Equal(tick, decoded.Tick);
        Assert.Equal(ulong.MaxValue, decoded.Count);
        Assert.Equal(int.MaxValue, decoded.Small);
    }

    [Theory]
    [InlineData("{\"tick\":\"1\",\"tick\":\"2\"}")]
    [InlineData("{\"tick\":\"0001\"}")]
    [InlineData("{\"tick\":1}")]
    [InlineData("{\"name\":null}")]
    [InlineData("\uFEFF{\"tick\":\"1\"}")]
    [InlineData("{\"tick\":\"1\",}")]
    [InlineData("{/*comment*/\"tick\":\"1\"}")]
    public void Default_connector_rejects_profile_violations(string json)
    {
        Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<Counters>());
    }

    [Fact]
    public void Default_connector_preserves_case_unknown_and_required_property_rules()
    {
        Assert.Equal(0, Payload("{\"TICK\":\"1\",\"future\":true}").FromJson<Counters>().Tick);
        Assert.Equal("default", Payload("{}").FromJson<Counters>().Name);
        Assert.ThrowsAny<JsonException>(() => Payload("{}").FromJson<RequiredName>());
    }

    [Theory]
    [InlineData("1e400")]
    [InlineData("-1e400")]
    public void Default_connector_and_server_reject_nonfinite_numbers(string json)
    {
        Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<double>());
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<double>(Encoding.UTF8.GetBytes(json))
        );
        Assert.Equal(1.25, Payload("1.25").FromJson<double>());
    }

    [Fact]
    public void Default_connector_rejects_single_precision_overflow()
    {
        Assert.ThrowsAny<JsonException>(() => Payload("1e39").FromJson<float>());
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<float>(Encoding.UTF8.GetBytes("1e39"))
        );
    }

    [Fact]
    public void Default_connector_uses_enum_names_and_rejects_implicit_custom_types()
    {
        Assert.Equal("\"Ready\"", Encoding.UTF8.GetString(State.Ready.ToJson().Payload.Span));
        Assert.ThrowsAny<JsonException>(() => Payload("0").FromJson<State>());
        Assert.ThrowsAny<JsonException>(() => Payload("\"ready\"").FromJson<State>());
        Assert.ThrowsAny<JsonException>(() => 1.25m.ToJson());
        Assert.ThrowsAny<JsonException>(() => DateTime.UnixEpoch.ToJson());
        Assert.ThrowsAny<JsonException>(() => Guid.Empty.ToJson());
        Assert.ThrowsAny<JsonException>(() => Payload("1.25").FromJson<decimal>());
        Assert.ThrowsAny<JsonException>(() =>
            Payload("\"1970-01-01T00:00:00Z\"").FromJson<DateTime>()
        );
        Assert.ThrowsAny<JsonException>(() =>
            Payload("\"00000000-0000-0000-0000-000000000000\"").FromJson<Guid>()
        );
    }

    private static ZlinkStreamEncodedPayload Payload(string json) =>
        new(ZlinkStreamCodec.Json, Encoding.UTF8.GetBytes(json));

    public sealed class Counters
    {
        public long Tick { get; set; }
        public ulong Count { get; set; }
        public int Small { get; set; }
        public string Name { get; set; } = "default";
    }

    public sealed class RequiredName
    {
        [JsonRequired]
        public string Name { get; set; } = string.Empty;
    }

    public enum State
    {
        Ready,
    }

    public sealed class ProfileSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            await Context.Client.Reply(payload.Decode<Counters>()).Async();
        }
    }
}
