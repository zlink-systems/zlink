using Microsoft.Extensions.Configuration;

using SpotActorTransfer.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Runtime.Spots;

namespace SpotActorTransfer.ActorNode;

internal static class ActorNodeHostFactory
{
    public static (WebApplication App, ServerOptions Options) Create(string[] args)
    {
        var options = ServerOptions.Parse(args, "actor-node");
        ZLinkSpotRetireTargetRuntime
            .PostPublicationBeforeNormalizationTestHook =
            options.CrashAtTargetCompleteGate
                ? static async cancellationToken =>
                {
                    Console.Error.WriteLine(
                        "aggregate_target_complete_gate");
                    Console.Error.Flush();
                    await Task.Delay(
                            Timeout.InfiniteTimeSpan,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
                : null;
        Directory.CreateDirectory(options.LogDir);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        var runtimeEvidence = new RuntimeEvidenceStore();
        var interruptionEvidence =
            new RelocationInterruptionEvidenceStore();
        builder.Logging.AddProvider(
            new ActorHandoffEvidenceLoggerProvider(runtimeEvidence, evidence));
        builder.WebHost.UseUrls(options.HttpUrl);
        var cleanupGates = new ActorCleanupGateStore(evidence);
        var relocationBlobs = new RelocationBlobObserver();
        var relocationMessageFlows =
            new RelocationMessageFlowEvidenceStore(options.Rid);
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton(runtimeEvidence);
        builder.Services.AddSingleton(interruptionEvidence);
        builder.Services.AddSingleton(relocationBlobs);
        builder.Services.AddSingleton(relocationMessageFlows);
        builder.Services.AddSingleton(new DomainStateStore(options.LogDir));
        builder.Services.AddSingleton<JoinedGateStore>();
        builder.Services.AddSingleton<TransferGateStore>();
        builder.Services.AddSingleton(cleanupGates);
        builder.Services.AddSingleton<ActorJoinTargetUseCase>();
        builder.Services.AddZLinkFramework(framework =>
        {
            framework.DefaultRequestTimeout = TimeSpan.FromMilliseconds(options.RequestTimeoutMilliseconds);
            // This E2E host is not started inside a memory-limited container.
            // Supply a deterministic finite limit so the default Auto HWM
            // contract does not depend on the developer or CI host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            // Normal diagnostics emits the received/replied Activity pairs used
            // by the relocation workload's public correlation assertion.
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            // The common ST-F4/F5 contract permits a short controller duration so
            // the E2E verifies cutoff semantics without coupling the scenario to
            // the independently tested owner-lease TTL.
            var redisStore = new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = options.RedisEndpoint;
                redis.KeyPrefix = options.RedisKeyPrefix;
            });
            framework.AddLocationStore(new CleanupGatedLocationStore(
                redisStore,
                cleanupGates));
            framework.AddRelocationStore(new ObservedRelocationStore(
                new ZLinkRedisRelocationStore(redis =>
                {
                    redis.ConnectionString = options.RedisEndpoint;
                    redis.KeyPrefix =
                        $"{options.RedisKeyPrefix}:relocation";
                }),
                relocationBlobs));
            var locations = framework.ConfigureLocations();
            // A third node keeps a bounded stale owner route so ST-I4 can
            // exercise Message Follow through the public global Actor ID API.
            // The cache remains five seconds shorter than Message Follow.
            locations.RouteCacheMaxAge = TimeSpan.FromSeconds(2);
            locations.MessageFollowDuration = TimeSpan.FromSeconds(7);
            locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
            locations.OwnerLeaseTtl = TimeSpan.FromSeconds(10);
            locations.PollingInterval = TimeSpan.FromMilliseconds(500);
            framework.AddHandlersFromAssemblyOf<TransferEntrySpot>();
            var mesh28 = framework.AddRouteMesh(SpotActorTransferNames.Mesh)
                .Listen(options.RouterEndpoint)
                .SetAdvertiseHost(options.RouterAdvertiseHost)
                .SetRoutingIdPrefix(options.Rid)
                .SetActorLimit(30_000)
                .SetSpotLimit(5_000);
            if (options.CallerOnly)
            {
                mesh28.Objects().Client();
                return;
            }
            mesh28.Objects().Server()
                .AddEntrySpot<TransferEntrySpot>()
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeStateful, factory => factory.PreserveStateWith<TransferActorRelocationAdapter>())
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeEmptyState, factory => factory.PreserveStateWith<TransferActorRelocationAdapter>())
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeNoAdapter, factory => factory.RecreateOnRelocation())
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeFailLeave, factory => factory.PreserveStateWith<TransferActorRelocationAdapter>())
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeFailTransferOut, factory => factory.PreserveStateWith<TransferActorRelocationAdapter>())
                .AddActorFactory<TransferActor, TransferActorFactory>(
                    SpotActorTransferNames.ActorTypeFailTransferIn, factory => factory.PreserveStateWith<TransferActorRelocationAdapter>())
                .AddSpotFactory<TransferUserSpot>(
                    SpotActorTransferNames.UserSpotType, factory => factory.DisableRelocation())
                .AddSpotFactory<RelocationPayloadUserSpot>(
                    SpotActorTransferNames.RelocationPayloadUserSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .PreserveStateWith<RelocationPayloadUserSpotAdapter>())
                .AddSpotFactory<RelocationPayloadPerActorUserSpot>(
                    SpotActorTransferNames
                        .RelocationPayloadPerActorUserSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.PerActor)
                        .RecreateOnRelocation())
                .AddSpotFactory<RelocationReadyUserSpot>(
                    SpotActorTransferNames.RelocationReadyUserSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .RelocationReadiness(
                            ZLinkSpotRelocationReadinessMode
                                .ApplicationSignaled)
                        .PreserveStateWith<
                            RelocationReadyUserSpotAdapter>())
                .AddSpotFactory<RelocationReadyDefaultUserSpot>(
                    SpotActorTransferNames
                        .RelocationReadyDefaultUserSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                        .RelocationReadiness(
                            ZLinkSpotRelocationReadinessMode
                                .ApplicationSignaled)
                        .RecreateOnRelocation())
                .AddInstanceSpotFactory<RelocationPayloadInstanceSpot>(
                    SpotActorTransferNames.RelocationPayloadInstanceSpotType, factory => factory.PreserveStateWith<RelocationPayloadInstanceSpotAdapter>());
        });
        var app = builder.Build();
        app.Lifetime.ApplicationStopped.Register(
            relocationMessageFlows.Dispose);
        app.Lifetime.ApplicationStopped.Register(
            interruptionEvidence.Dispose);
        return (app, options);
    }
}
