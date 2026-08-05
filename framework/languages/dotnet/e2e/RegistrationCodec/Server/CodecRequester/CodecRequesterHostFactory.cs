using Microsoft.Extensions.Configuration;

using Google.Protobuf.WellKnownTypes;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using RegistrationCodec.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.E2E.Diagnostics;

namespace RegistrationCodec.Server.CodecRequester;

internal static class CodecRequesterHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = CodecRequesterOptions.Parse(args);
        Directory.CreateDirectory(options.LogDir);

        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(options);
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.Codecs.Use(ZLinkProtobufCodec.Default);
            framework.Codecs.Use(ZLinkMessagePackCodec.Default);
            var mesh = framework.AddRouteMesh(RegistrationCodecNames.Channel)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingId(RoutingId.From(options.Rid));
            mesh.Channel(RegistrationCodecNames.Channel).Client();
            mesh.PeerConnections.Connect(options.ChannelEndpoint);
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Rid }));
        app.MapGet("/topology/ready", (IZLinkRouteMeshRuntime runtime) =>
        {
            var snapshot = runtime.GetStatus(RegistrationCodecNames.Channel);
            return Results.Ok(new
            {
                ready = snapshot.Peers.Any(static peer =>
                            peer.State == ZLinkPeerState.Ready)
                        && snapshot.Channels.Any(static channel =>
                            channel.ChannelName == RegistrationCodecNames.Channel
                            && channel.IsReady)
            });
        });
        app.MapPost("/codec/protobuf/request", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(RegistrationCodecNames.Channel,
                        new StringValue { Value = "rc-b5" })
                    .Timeout(TimeSpan.FromSeconds(2))
                    .Async<StringValue>(cancellationToken);
                return Results.Ok(new CodecMismatchProbeRes(false, null, reply.Value));
            }
            catch (Exception ex)
            {
                return Results.Ok(new CodecMismatchProbeRes(true, ex.GetType().Name, null));
            }
        });
        app.MapPost("/codec/json/request", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(RegistrationCodecNames.Channel,
                    new JsonEchoReq("rc-b5-json"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/auto", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoAutoReq("rc-a1"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoAutoMsg("cmd-rc-a1", "rc-a1-send"))
                .Async(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/attribute", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoAttrReq("rc-a2"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoAttrMsg("cmd-rc-a2", "rc-a2-send"))
                .Async(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/manual", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoManualReq("rc-a3"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoManualMsg("cmd-rc-a3", "rc-a3-send"))
                .Async(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/di-filter-order", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var first = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoDiReq("rc-a4-1"))
                .Async<EchoRes>(cancellationToken);
            var second = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new EchoDiReq("rc-a4-2"))
                .Async<EchoRes>(cancellationToken);
            return Results.Ok(new[] { first, second });
        });
        app.MapPost("/codec/roundtrip", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var json = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new JsonEchoReq("rc-b1"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new JsonEchoMsg("cmd-rc-b1", "rc-b1-send"))
                .Async(cancellationToken);
            var protobuf = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new Google.Protobuf.WellKnownTypes.StringValue { Value = "rc-b2" })
                .Async<Google.Protobuf.WellKnownTypes.StringValue>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new Google.Protobuf.WellKnownTypes.StringValue { Value = "rc-b2-send" })
                .Async(cancellationToken);
            var packed = await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    new PackedEchoReq { Value = "rc-b3" })
                .Async<PackedEchoReq>(cancellationToken);
            await channel.SendToChannel(
                    RegistrationCodecNames.Channel,
                    new PackedEchoMsg { CommandId = "cmd-rc-b3", Value = "rc-b3-send" })
                .Async(cancellationToken);
            return Results.Ok(new CodecScenarioRes(json, protobuf.Value, packed.Value));
        });
        app.MapPost("/codec/json-golden", async (
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var request = new JsonGoldenReq(
                "Ada Lovelace",
                "ready",
                -9_223_372_036_854_775_000L,
                [0x00, 0x7f, 0x80, 0xff],
                2_147_000_001,
                0.125,
                null);
            return Results.Ok(await channel.RequestToChannel(
                    RegistrationCodecNames.Channel,
                    request)
                .Async<JsonGoldenRes>(cancellationToken));
        });
        return app;
    }
}

internal sealed record CodecScenarioRes(
    EchoRes Json,
    string ProtobufValue,
    string MessagePackValue);
