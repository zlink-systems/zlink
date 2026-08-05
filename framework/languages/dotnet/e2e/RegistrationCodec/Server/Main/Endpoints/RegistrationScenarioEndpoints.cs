using Google.Protobuf.WellKnownTypes;
using RegistrationCodec.Shared;
using Zlink.Framework.Contracts.Channels;

namespace RegistrationCodec.Server.Main.Endpoints;

internal static class RegistrationScenarioEndpoints
{
    public static WebApplication MapRegistrationScenarioEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapPost("/registration/auto", async (IZLinkRouteClient channel, CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(RegistrationCodecNames.Channel, new EchoAutoReq("rc-a1"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(RegistrationCodecNames.Channel, new EchoAutoMsg("cmd-rc-a1", "rc-a1-send")).Async(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/attribute",
            async (IZLinkRouteClient channel, CancellationToken cancellationToken) =>
            {
                var reply = await channel.RequestToChannel(RegistrationCodecNames.Channel, new EchoAttrReq("rc-a2"))
                    .Async<EchoRes>(cancellationToken);
                await channel.SendToChannel(RegistrationCodecNames.Channel, new EchoAttrMsg("cmd-rc-a2", "rc-a2-send")).Async(cancellationToken);
                return Results.Ok(reply);
            });
        app.MapPost("/registration/manual", async (IZLinkRouteClient channel, CancellationToken cancellationToken) =>
        {
            var reply = await channel.RequestToChannel(RegistrationCodecNames.Channel, new EchoManualReq("rc-a3"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(RegistrationCodecNames.Channel,
                    new EchoManualMsg("cmd-rc-a3", "rc-a3-send")).Async(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/registration/di-filter-order",
            async (IZLinkRouteClient channel, CancellationToken cancellationToken) =>
            {
                var first = await channel.RequestToChannel(RegistrationCodecNames.Channel, new EchoDiReq("rc-a4-1"))
                    .Async<EchoRes>(cancellationToken);
                var second = await channel.RequestToChannel(RegistrationCodecNames.Channel, new EchoDiReq("rc-a4-2"))
                    .Async<EchoRes>(cancellationToken);
                return Results.Ok(new[] { first, second });
            });
        app.MapPost("/codec/roundtrip", async (IZLinkRouteClient channel, CancellationToken cancellationToken) =>
        {
            var json = await channel.RequestToChannel(RegistrationCodecNames.Channel, new JsonEchoReq("rc-b1"))
                .Async<EchoRes>(cancellationToken);
            await channel.SendToChannel(RegistrationCodecNames.Channel, new JsonEchoMsg("cmd-rc-b1", "rc-b1-send")).Async(cancellationToken);

            var protobuf = await channel
                .RequestToChannel(RegistrationCodecNames.Channel, new StringValue { Value = "rc-b2" })
                .Async<StringValue>(cancellationToken);
            await channel.SendToChannel(RegistrationCodecNames.Channel, new StringValue { Value = "rc-b2-send" }).Async(cancellationToken);

            var packed = await channel
                .RequestToChannel(RegistrationCodecNames.Channel, new PackedEchoReq { Value = "rc-b3" })
                .Async<PackedEchoReq>(cancellationToken);
            await channel.SendToChannel(RegistrationCodecNames.Channel,
                    new PackedEchoMsg { CommandId = "cmd-rc-b3", Value = "rc-b3-send" }).Async(cancellationToken);

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
            return Results.Ok(await channel
                .RequestToChannel(RegistrationCodecNames.Channel, request)
                .Async<JsonGoldenRes>(cancellationToken));
        });
        return app;
    }
}

internal sealed record CodecScenarioRes(EchoRes Json, string ProtobufValue, string MessagePackValue);
