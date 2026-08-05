using PubSub.Server.Subscriber.Configuration;
using PubSub.Server.Subscriber.Handlers;
using PubSub.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.E2E.Diagnostics;

namespace PubSub.Server.Subscriber;

internal static class SubscriberHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = SubscriberOptions.Parse(args);
        var builder = HostFactorySupport.CreateBuilder(args, options.HttpUrl, options.LogDir);
        builder.Services.AddSingleton(options);
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton(new HandlerDelayOptions(options.HandlerDelayMs));
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase is not ("error" or "dropped")) return;
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
            // Classic fanout uses no location store (config-3): the
            // subscriber names the publisher endpoint explicitly.
            var subscriber = framework.AddFanoutChannel(PubSubNames.Channel)
                .Connect(options.PublisherEndpoint);
            subscriber.AddHandler<EventMsgHandler, EventMsg>("EventMsg");
        });
        var app = builder.Build();
        app.MapOperationalEndpoints("subscriber", options.Rid);
        return app;
    }

}
