using System.Globalization;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

internal static class ActorDestroyHost
{
    internal const string ActorType = "cross-lang-actor-destroy-type";

    internal static void Configure(
        IServiceCollection services,
        TestHostOptions options,
        TestHostEventSink sink
    )
    {
        if (
            options.ActorDestroyScenario
            is not ("a" or "b" or "c" or "d" or "e" or "f" or "admissionCheck")
        )
            throw new InvalidOperationException("Invalid --actor-destroy-scenario.");

        var meshName = Required(options.MeshName, "mesh-name");
        var nodeRid = RoutingId.From(Required(options.NodeRid, "node-rid"));
        _ = Required(options.PeerRid, "peer-rid");
        var bindEndpoint = Required(options.BindEndpoint, "bind-endpoint");
        var redisEndpoint = Required(options.RedisEndpoint, "redis-endpoint");
        var prefix = Required(options.RedisKeyPrefix, "redis-key-prefix");
        var actorId = Required(options.ActorId, "actor-id");
        var eventPath = Required(options.EventFilePath, "event-file");
        var runId = Required(options.ActorDestroyRunId, "actor-destroy-run-id");
        _ = Required(options.ActorDestroyControlPrefix, "actor-destroy-control-prefix");
        _ = Required(options.StartFilePath, "start-file");

        var owner = options.Mode == "actor-destroy-owner";
        var events = new ActorDestroyEvents(sink, runId, options.ActorDestroyScenario, owner);
        var store = new ActorDestroyStore(
            new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = redisEndpoint;
                redis.KeyPrefix = $"{prefix}:location";
            }),
            events,
            actorId,
            meshName
        );
        services.AddSingleton(options);
        services.AddSingleton(events);
        services.AddSingleton(_ => store);
        services.AddSingleton(provider => new TestHostMessageFlowListener(
            eventPath + ".flow",
            provider.GetRequiredService<TestHostEventSink>()
        ));
        services.AddZLinkFramework(framework =>
        {
            events.RequestTimeout = framework.DefaultRequestTimeout;
            framework.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.AddLocationStore(store);
            var mesh = framework
                .AddRouteMesh(meshName)
                .Listen(bindEndpoint)
                .SetRoutingId(nodeRid)
                .SetPlacementWeight(owner ? 100 : 0);
            if (options.AdvertiseHost is { } advertiseHost)
                mesh.SetAdvertiseHost(advertiseHost);
            mesh.Channel(meshName).Server();
            var objects = mesh.Objects().Server();
            objects.AddEntrySpot<ActorDestroyEntrySpot>();
            objects.AddActorFactory<ActorDestroyActor, ActorDestroyFactory>(
                ActorType,
                factory => factory.DisableRelocation()
            );
        });
        services.AddHostedService<ActorDestroyCommands>();
    }

    private static string Required(string? value, string name) =>
        !string.IsNullOrWhiteSpace(value)
            ? value
            : throw new InvalidOperationException($"Actor destroy requires --{name}.");
}

internal sealed class ActorDestroyEvents(
    TestHostEventSink sink,
    string runId,
    string scenario,
    bool owner
)
{
    public string Scenario { get; } = scenario;
    public bool Owner { get; } = owner;
    public TimeSpan RequestTimeout { get; set; }

    public void Write(string name, params (string Name, object? Value)[] fields) =>
        WriteAt(DateTimeOffset.UtcNow, name, fields);

    public void WriteAt(
        DateTimeOffset timestamp,
        string name,
        params (string Name, object? Value)[] fields
    )
    {
        var time = (timestamp.UtcTicks - DateTimeOffset.UnixEpoch.UtcTicks) / 10;
        var row = FormattableString.Invariant(
            $"actor-destroy|run={Escape(runId)}|event={name}|timeUs={time}"
        );
        foreach (var field in fields)
            row +=
                $"|{field.Name}={Escape(Convert.ToString(field.Value, CultureInfo.InvariantCulture) ?? "")}";
        sink.Append(row);
    }

    private static string Escape(string value) =>
        value
            .Replace("%", "%25", StringComparison.Ordinal)
            .Replace("|", "%7C", StringComparison.Ordinal)
            .Replace("\r", "%0D", StringComparison.Ordinal)
            .Replace("\n", "%0A", StringComparison.Ordinal);
}

internal sealed class ActorDestroyActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
}

internal sealed class ActorDestroyFactory(ActorDestroyEvents events)
    : IZLinkActorFactory<ActorDestroyActor>
{
    public ValueTask<ActorDestroyActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    )
    {
        if (!events.Owner)
            events.Write("newFactoryStarted");
        return ValueTask.FromResult(new ActorDestroyActor(context));
    }
}

internal sealed class ActorDestroyEntrySpot(IZLinkEntrySpotContext context)
    : IZLinkEntrySpot<ActorDestroyActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddHandler<ActorDestroyProbeHandler>();

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        ActorDestroyActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

    public ValueTask OnJoinedActorAsync(
        ActorDestroyActor actor,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(
        ActorDestroyActor actor,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;
}

[ZLinkPacket("ActorDestroyProbe")]
internal sealed record ActorDestroyProbeReq(string Label);

internal sealed class ActorDestroyProbeHandler(ActorDestroyEvents events)
    : IZLinkEntrySpotActorRequestHandler<
        ActorDestroyEntrySpot,
        ActorDestroyActor,
        ActorDestroyProbeReq,
        string
    >,
        IDisposable
{
    public void Dispose() => events.Write("cleanup");

    public ValueTask<string> HandleAsync(
        ActorDestroyEntrySpot spot,
        ActorDestroyActor actor,
        IZLinkMessageContext context,
        ActorDestroyProbeReq request,
        CancellationToken cancellationToken
    )
    {
        events.Write("probeHandled", ("label", request.Label));
        return ValueTask.FromResult(
            actor.Context.ObjectGeneration.ToString(CultureInfo.InvariantCulture)
        );
    }
}
