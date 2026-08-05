using Microsoft.Extensions.Configuration;

using Google.Protobuf.WellKnownTypes;
using Systems.Zlink;
using RegistrationCodec.Server.InvalidDuplicate.Handlers;
using RegistrationCodec.Server.InvalidDuplicate.Infrastructure;
using RegistrationCodec.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.E2E.Diagnostics;

namespace RegistrationCodec.Server.InvalidDuplicate;

public static class RegistrationCodecServerHostFactory
{
    public static WebApplication Create(string[] args)
    {
        return CreateWithMode(args, null);
    }

    private static WebApplication CreateWithMode(
        string[] args,
        Action<WebApplication, ServerOptions>? configureApp)
    {
        var options = ServerOptions.Parse(args) with { InvalidMode = "duplicate" };
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
        builder.Services.AddSingleton(new EvidenceStore(options.EvidenceFile));
        builder.Services.AddSingleton<SingletonProbe>();
        builder.Services.AddScoped<ScopedProbe>();
        builder.Services.AddSingleton<FirstFilter>();
        builder.Services.AddSingleton<SecondFilter>();
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
            if (options.CodecMode != "json-only")
            {
                framework.Codecs.Use(ZLinkProtobufCodec.Default);
                framework.Codecs.Use(ZLinkMessagePackCodec.Default);
            }

            framework.AddHandlersFromAssemblyOf<EchoAutoRequestHandler>();
            framework.UseFilter<FirstFilter>();
            framework.UseFilter<SecondFilter>();

            var mesh = framework.AddRouteMesh(RegistrationCodecNames.Channel)
                .Listen(Require(options.ChannelEndpoint, "ChannelEndpoint"))
                .SetRoutingId(RoutingId.From(options.Rid));
            var channel = mesh.Channel(RegistrationCodecNames.Channel).Server();
            channel.AddRequestHandler<EchoAutoRequestHandler, EchoAutoReq, EchoRes>();
            channel.AddSendHandler<EchoAutoCommandHandler, EchoAutoMsg>();
            channel.AddRequestHandler<AttributeHandlers, EchoAttrReq, EchoRes>("EchoAttr");
            channel.AddSendHandler<AttributeHandlers, EchoAttrMsg>("EchoAttrMsg");
            channel.AddRequestHandler<EchoManualRequestHandler, EchoManualReq, EchoRes>("EchoManual");
            channel.AddSendHandler<EchoManualCommandHandler, EchoManualMsg>("EchoManualMsg");
            channel.AddRequestHandler<JsonEchoRequestHandler, JsonEchoReq, EchoRes>("EchoJson");
            channel.AddSendHandler<JsonEchoCommandHandler, JsonEchoMsg>("EchoJsonMsg");
            channel.AddRequestHandler<ProtobufEchoRequestHandler, StringValue, StringValue>();
            channel.AddSendHandler<ProtobufEchoCommandHandler, StringValue>();
            channel.AddRequestHandler<MessagePackEchoRequestHandler, PackedEchoReq, PackedEchoReq>("EchoMessagePack");
            channel.AddSendHandler<MessagePackEchoCommandHandler, PackedEchoMsg>("EchoMessagePackMsg");
            channel.AddRequestHandler<DiEchoRequestHandler, EchoDiReq, EchoRes>("EchoDi");

            if (options.InvalidMode == "duplicate")
                channel.AddRequestHandler<DuplicateEchoRequestHandler, EchoManualReq, EchoRes>("EchoManual");
        });

        var app = builder.Build();
        app.MapOperationalEndpoints(options);
        configureApp?.Invoke(app, options);
        return app;
    }

    private static string Require(string? value, string name)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{name} is required.")
            : value;
    }
}
