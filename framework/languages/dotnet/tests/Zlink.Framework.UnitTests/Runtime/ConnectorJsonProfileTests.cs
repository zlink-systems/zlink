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
    [InlineData("\"0\"", 0L)]
    [InlineData("\"-1\"", -1L)]
    [InlineData("\"9223372036854775807\"", long.MaxValue)]
    [InlineData("\"-9223372036854775808\"", long.MinValue)]
    [InlineData("\"\\u0031\"", 1L)]
    [InlineData("\"\\u002d1\"", -1L)]
    public void Connector_and_server_accept_canonical_signed_integer_strings(
        string json,
        long expected
    )
    {
        Assert.Equal(expected, Payload(json).FromJson<long>());
        Assert.Equal(
            expected,
            ZLinkFrameworkJsonPayloadCodec.Deserialize<long>(Encoding.UTF8.GetBytes(json))
        );
        Assert.Equal(
            expected,
            Payload(Encoding.UTF8.GetString(expected.ToJson().Payload.Span)).FromJson<long>()
        );
    }

    [Theory]
    [InlineData("\"0\"", 0UL)]
    [InlineData("\"9223372036854775808\"", 9223372036854775808UL)]
    [InlineData("\"18446744073709551615\"", ulong.MaxValue)]
    [InlineData("\"\\u0031\"", 1UL)]
    public void Connector_and_server_accept_canonical_unsigned_integer_strings(
        string json,
        ulong expected
    )
    {
        Assert.Equal(expected, Payload(json).FromJson<ulong>());
        Assert.Equal(
            expected,
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ulong>(Encoding.UTF8.GetBytes(json))
        );
        Assert.Equal(
            expected,
            Payload(Encoding.UTF8.GetString(expected.ToJson().Payload.Span)).FromJson<ulong>()
        );
    }

    [Theory]
    [InlineData("\"\"")]
    [InlineData("\"-\"")]
    [InlineData("\"-0\"")]
    [InlineData("\"+1\"")]
    [InlineData("\"00\"")]
    [InlineData("\"01\"")]
    [InlineData("\"-01\"")]
    [InlineData("\" 1\"")]
    [InlineData("\"1 \"")]
    [InlineData("\"1e0\"")]
    [InlineData("\"1.0\"")]
    [InlineData("\"١\"")]
    [InlineData("\"１\"")]
    [InlineData("\"1\\u0000\"")]
    [InlineData("\"\\u002b1\"")]
    [InlineData("\"\\u00301\"")]
    [InlineData("\"18446744073709551616\"")]
    [InlineData("\"-9223372036854775809\"")]
    [InlineData("1")]
    [InlineData("null")]
    public void Connector_and_server_reject_noncanonical_integer_strings(string json)
    {
        Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<long>());
        Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<ulong>());
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<long>(Encoding.UTF8.GetBytes(json))
        );
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ulong>(Encoding.UTF8.GetBytes(json))
        );
    }

    [Fact]
    public void Connector_and_server_preserve_each_integer_range()
    {
        const string signedOverflow = "\"9223372036854775808\"";
        const string unsignedUnderflow = "\"-1\"";
        Assert.ThrowsAny<JsonException>(() => Payload(signedOverflow).FromJson<long>());
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<long>(Encoding.UTF8.GetBytes(signedOverflow))
        );
        Assert.ThrowsAny<JsonException>(() => Payload(unsignedUnderflow).FromJson<ulong>());
        Assert.ThrowsAny<JsonException>(() =>
            ZLinkFrameworkJsonPayloadCodec.Deserialize<ulong>(
                Encoding.UTF8.GetBytes(unsignedUnderflow)
            )
        );
    }

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

    [Fact]
    public void Default_connector_uses_enum_names_and_rejects_numeric_enum_properties()
    {
        var expected = new EnumPayload(State.Ready);
        var encoded = expected.ToJson();

        Assert.Equal("{\"state\":\"Ready\"}", Encoding.UTF8.GetString(encoded.Payload.Span));
        Assert.Equal(expected, Payload("{\"state\":\"Ready\"}").FromJson<EnumPayload>());
        Assert.ThrowsAny<JsonException>(() => Payload("{\"state\":1}").FromJson<EnumPayload>());
        Assert.ThrowsAny<JsonException>(() => Payload("{\"state\":\"1\"}").FromJson<EnumPayload>());
    }

    [Theory]
    [InlineData("plain-duplicate", "{\"seq\":1,\"seq\":2}", true)]
    [InlineData("escaped-current", "{\"seq\":1,\"s\\u0065q\":2}", true)]
    [InlineData("escaped-previous", "{\"s\\u0065q\":1,\"seq\":2}", true)]
    [InlineData("both-escaped", "{\"\\u0073eq\":1,\"s\\u0065q\":2}", true)]
    [InlineData("unicode", "{\"é\":1,\"\\u00e9\":2}", true)]
    [InlineData("surrogate-pair", "{\"😀\":1,\"\\ud83d\\ude00\":2}", true)]
    [InlineData("unknown-nested-duplicate", "{\"seq\":1,\"unknown\":{\"x\":1,\"x\":2}}", true)]
    [InlineData("array-nested-duplicate", "[{\"x\":1,\"x\":2}]", true)]
    [InlineData("nested-separate-name", "{\"x\":{\"x\":1}}", false)]
    [InlineData("sibling-separate-name", "[{\"x\":1},{\"x\":2}]", false)]
    [InlineData("case-sensitive", "{\"x\":1,\"X\":2}", false)]
    [InlineData("unicode-no-normalization", "{\"é\":1,\"e\\u0301\":2}", false)]
    [InlineData("bom", "\uFEFF{}", true)]
    [InlineData("infinite-unknown", "{\"unknown\":1e400}", true)]
    [InlineData("finite", "{\"x\":1.25e10}", false)]
    [InlineData("trailing-comma", "{\"x\":1,}", true)]
    [InlineData("comment", "{/*comment*/\"x\":1}", true)]
    [InlineData("empty-key-duplicate", "{\"\":1,\"\":2}", true)]
    [InlineData("trailing-data", "{} {}", true)]
    [InlineData("root-scalar", "123", false)]
    [InlineData("root-array", "[1,[2],{\"x\":3}]", false)]
    [InlineData("sibling-properties", "{\"a\":{\"x\":1},\"b\":{\"x\":2},\"x\":3}", false)]
    [InlineData("duplicate-after-child", "{\"x\":{\"a\":1},\"x\":2}", true)]
    public void Connector_and_server_preserve_document_validation(
        string name,
        string json,
        bool reject
    )
    {
        _ = name;
        if (reject)
        {
            Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<JsonElement>());
            Assert.ThrowsAny<JsonException>(() =>
                ZLinkFrameworkJsonPayloadCodec.Deserialize<JsonElement>(
                    Encoding.UTF8.GetBytes(json)
                )
            );
        }
        else
        {
            var connector = Payload(json).FromJson<JsonElement>();
            var server = ZLinkFrameworkJsonPayloadCodec.Deserialize<JsonElement>(
                Encoding.UTF8.GetBytes(json)
            );
            Assert.Equal(json, connector.GetRawText());
            Assert.Equal(json, server.GetRawText());
        }
    }

    [Theory]
    [InlineData("{}", true)]
    [InlineData("{\"seq\":1,\"unknown\":2}", false)]
    public void Connector_and_server_preserve_required_validation(string json, bool reject)
    {
        if (reject)
        {
            Assert.ThrowsAny<JsonException>(() => Payload(json).FromJson<RequiredSequence>());
            Assert.ThrowsAny<JsonException>(() =>
                ZLinkFrameworkJsonPayloadCodec.Deserialize<RequiredSequence>(
                    Encoding.UTF8.GetBytes(json)
                )
            );
        }
        else
        {
            Assert.Equal(1, Payload(json).FromJson<RequiredSequence>().Seq);
            Assert.Equal(
                1,
                ZLinkFrameworkJsonPayloadCodec
                    .Deserialize<RequiredSequence>(Encoding.UTF8.GetBytes(json))!
                    .Seq
            );
        }
    }

    [Theory]
    [InlineData(64, false)]
    [InlineData(65, true)]
    public void Connector_and_server_preserve_reader_depth_limit(int depth, bool reject)
    {
        var json =
            string.Concat(Enumerable.Repeat("{\"x\":", depth)) + "0" + new string('}', depth);
        Connector_and_server_preserve_document_validation("object-depth", json, reject);
        json = new string('[', depth) + "0" + new string(']', depth);
        Connector_and_server_preserve_document_validation("array-depth", json, reject);
    }

    public sealed class RequiredSequence
    {
        [JsonRequired]
        public int Seq { get; set; }
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

    public sealed record EnumPayload(State State);

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
