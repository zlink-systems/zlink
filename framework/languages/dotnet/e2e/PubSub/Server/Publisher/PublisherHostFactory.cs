using PubSub.Server.Publisher.Configuration;
using PubSub.Server.Publisher.Endpoints;
using PubSub.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.E2E.Diagnostics;

namespace PubSub.Server.Publisher;

internal static class PublisherHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = PublisherOptions.Parse(args);
        var builder = HostFactorySupport.CreateBuilder(args, options.HttpUrl, options.LogDir);
        builder.Services.AddSingleton(options);
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase != "error") return;
                evidence.Add(
                    "dispatch-error"
                    + $"|surface={flow.Surface}"
                    + $"|kind={flow.MessageKind}"
                    + $"|reason={flow.Reason}"
                    + $"|action={flow.Action}"
                    + $"|packet={flow.PacketName ?? "<null>"}"
                    + $"|channel={flow.ChannelName ?? "<null>"}"
                    + $"|topic={flow.Topic ?? "<null>"}");
            }));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.AddFanoutChannel(PubSubNames.Channel)
                .EnablePublisher(options.PublisherEndpoint);
        });
        var app = builder.Build();
        app.MapOperationalEndpoints("publisher", options.Rid);
        app.MapPublisherEndpoints();
        return app;
    }

}
