using System.Text.Json;
using System.Text.Json.Serialization;
using Systems.Zlink;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;

namespace ZLink.Framework.Perf;

public static class ServerApplication
{
    private static readonly JsonSerializerOptions ServerJson = CreateServerJson();
    private static JsonSerializerOptions CreateServerJson()
    {
        var options = new JsonSerializerOptions(PerfJson.Options);
        options.Converters.Add(new RoutingIdObservationConverter());
        return options;
    }
    public static RoleConfig ReadConfig(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config") throw new ArgumentException("Server requires --config <file> only.");
        var config = PerfJson.Read<RoleConfig>(File.ReadAllText(args[1]));
        config.workload.ValidateCcu();
        if (config.workload.requestPayloadBytes != 64 || config.workload.responsePayloadBytes != 4096 || config.workload.sendPayloadBytes != 4096)
            throw new ArgumentException("Standard payload is request64/response4096/send4096 logical bytes.");
        if (config.workload.applicationDeadlineMs > config.workload.settleTimeoutMs || config.workload.applicationDeadlineMs <= 0)
            throw new ArgumentException("Application deadline must be positive and within settle bound.");
        if (!double.IsFinite(config.workload.durationSeconds) || config.workload.durationSeconds <= 0 ||
            !double.IsFinite(config.workload.warmupSeconds) || config.workload.warmupSeconds <= 0 || config.workload.inflight <= 0)
            throw new ArgumentException("Duration, warmup and in-flight must be positive finite values.");
        using var matrix = JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "matrix.json")));
        if (!matrix.RootElement.GetProperty("cells").EnumerateArray().Any(cell =>
            cell.GetProperty("scenario").GetString() == config.scenario && cell.GetProperty("mode").GetString() == config.mode &&
            cell.GetProperty("terminal").GetString() == config.terminal))
            throw new ArgumentException("Scenario, mode and terminal must match a common matrix cell.");
        Histogram.ValidateFixture();
        if (new Uri(config.metricsUrl).Port == new Uri(config.applicationTriggerUrl).Port)
            throw new ArgumentException("Admin and application trigger require separate listeners.");
        return config;
    }
    public static WebApplicationBuilder Builder(RoleConfig config, Action<IZLinkFrameworkOptions> configure)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.WebHost.UseUrls(config.metricsUrl, new Uri(config.applicationTriggerUrl).GetLeftPart(UriPartial.Authority));
        builder.Services.AddSingleton(config);
        builder.Services.AddSingleton(new Measurement(config, config.source));
        builder.Services.AddSingleton<PublicMetricCollector>();
        if (config.diagnostics is not null)
        {
            if (config.diagnostics.level != "Normal") throw new ArgumentException("Diagnostic runs use Normal message-flow tracing.");
            builder.Services.AddSingleton(_ => new MessageFlowFileListener(config.diagnostics.flowFile));
        }
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.DefaultRequestTimeout = TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs);
            options.DefaultSocketSendTimeout = TimeSpan.FromMilliseconds(config.workload.socketSendTimeoutMs);
            options.ConfigureDispatch().Diagnostics.SetLevel(config.diagnostics is null ? ZLinkDiagnosticsLevel.Off : ZLinkDiagnosticsLevel.Normal);
            options.ConfigureNetwork().BindHost = "127.0.0.1";
            options.ConfigureNetwork().AdvertiseHost = "127.0.0.1";
            if (config.store is { } store)
            {
                if (store.provider != "redis") throw new ArgumentException("Standard object/discovery workloads require Redis.");
                options.AddLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
                    { ConnectionString = store.endpoint, KeyPrefix = store.@namespace }));
            }
            options.Worker.MinThreads = config.workload.workerPoolSize;
            options.Worker.MaxThreads = config.workload.workerPoolSize;
            options.Worker.IdleTimeout = TimeSpan.FromMilliseconds(60000);
            configure(options);
        });
        return builder;
    }
    public static void Map(WebApplication app, Func<Task>? workload = null, Func<CancellationToken, Task>? prepare = null)
    {
        var config = app.Services.GetRequiredService<RoleConfig>();
        var measurement = app.Services.GetRequiredService<Measurement>();
        var runtime = app.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        var provider = app.Services.GetRequiredService<PublicMetricCollector>();
        if (config.diagnostics is not null) _ = app.Services.GetRequiredService<MessageFlowFileListener>();
        measurement.SamplePublicState = () =>
        {
            var status = runtime.Status;
            return new { observedTicks = DecimalText.Of(PerfClock.Now), status.State, status.IsReady,
                status.AcceptingWork, status.Capacity.ApplicationJobQueue.PressureState };
        };
        app.Use(async (context, next) =>
        {
            var correctPort = context.Request.Path.StartsWithSegments("/perf") ? new Uri(config.metricsUrl).Port :
                new Uri(config.applicationTriggerUrl).Port;
            if (context.Connection.LocalPort != correctPort) { context.Response.StatusCode = 404; return; }
            await next(context);
        });
        app.MapGet("/perf/ready", () => Json(Ready(app.Services)));
        app.MapGet("/perf/stats", () => Json(measurement.Snapshot(PublicStatus(app.Services)) with { publicMetrics = provider.Snapshot() }));
        app.MapPost("/perf/reset", async (HttpContext context) =>
        {
            try
            {
                var request = await Read<ResetRequest>(context);
                var reply = measurement.Reset(request, () => { runtime.ResetCapacityMetrics(); return runtime.Status.Capacity.MeasurementEpoch; });
                return Json(reply, reply.ok ? 200 : 409);
            }
            catch (JsonException error) { return Json(new { reason = error.Message }, 400); }
        });
        var prepareGate = new object();
        Task? prepareTask = null;
        app.MapPost("/app/perf/prepare", async (HttpContext context) =>
        {
            var request = await Read<PerfTriggerRequest>(context);
            if (request.runId != config.runId || request.cellId != config.cellId || request.phase != "setup" || request.resetSeq != "0")
                return Json(new { ok = false, reason = "Preparation identity differs." }, 409);
            if (prepare is not null)
            {
                Task pending;
                lock (prepareGate) pending = prepareTask ??= prepare(app.Lifetime.ApplicationStopping);
                await pending;
            }
            return Json(new { ok = !measurement.HasErrors, ready = Ready(app.Services) });
        });
        app.MapPost("/app/perf/start", async (HttpContext context) =>
        {
            try
            {
                var request = await Read<PerfTriggerRequest>(context);
                DecimalText.U64(request.resetSeq);
                var ready = Ready(app.Services);
                if (!ready.ready) return Json(new { reason = "Readiness evidence is incomplete.", ready }, 409);
                var reply = measurement.Start(request, workload);
                return Json(reply, reply.accepted ? 200 : 409);
            }
            catch (JsonException error) { return Json(new { reason = error.Message }, 400); }
        });
    }
    private static async Task<T> Read<T>(HttpContext context)
    {
        using var reader = new StreamReader(context.Request.Body);
        var value = PerfJson.Read<T>(await reader.ReadToEndAsync(context.RequestAborted));
        if (value is ResetRequest reset && (reset.runId is null || reset.cellId is null) ||
            value is Identity identity && (identity.runId is null || identity.cellId is null || identity.phase is null))
            throw new JsonException("Identity text fields must be non-null JSON strings.");
        return value;
    }
    public static IResult Json<T>(T value, int status = 200) => Results.Json(value, ServerJson, statusCode: status);
    public static object PublicStatus(IServiceProvider services)
    {
        var config = services.GetRequiredService<RoleConfig>();
        var host = services.GetRequiredService<IZLinkFrameworkRuntime>().Status;
        if (config.mode == "publish" && !config.source) return new { host, fanout = services.GetRequiredService<IZLinkFanoutRuntime>().GetStatus(config.channelName!) };
        if (config.topology == "routemesh" && config.mode != "publish" || config.scenario == "cs-remote-session-actor-echo" && config.objectRole == "Client") return new { host, routeMesh = services.GetRequiredService<IZLinkRouteMeshRuntime>().GetStatus(config.meshName!) };
        if (config.topology == "clientserver") return new { host, clientServer = services.GetRequiredService<IZLinkClientServerRuntime>().GetStatus(config.channelName!) };
        return new { host };
    }
    public static PerfReady Ready(IServiceProvider services)
    {
        var config = services.GetRequiredService<RoleConfig>();
        var measurement = services.GetRequiredService<Measurement>();
        var host = services.GetRequiredService<IZLinkFrameworkRuntime>().Status;
        var infrastructure = host.IsReady;
        if (config.topology == "routemesh" && config.mode != "publish" || config.scenario == "cs-remote-session-actor-echo" && config.objectRole == "Client")
        {
            var mesh = services.GetRequiredService<IZLinkRouteMeshRuntime>().GetStatus(config.meshName!);
            // IsReady also permits local traffic with no peers (topology monitoring §4).
            // This caller's object target is on the separate server role, so §16.1
            // infrastructure readiness requires a ready remote connection before its probe.
            var remoteObjectTarget = config.objectRole == "Client" &&
                (config.source && !config.scenario.StartsWith("cs-", StringComparison.Ordinal)
                    || config.scenario == "cs-remote-session-actor-echo");
            infrastructure &= !remoteObjectTarget || mesh.ReadyPeerCount > 0;
            // Channel messaging §3: RouteMesh excludes the sending node itself from candidates.
            // Only the source needs a selectable remote target; the receiver proves dispatch by echo.
            infrastructure &= mesh.IsReady && (!(config.source && (config.role == "spot" && config.scenario.StartsWith("s2s-spot", StringComparison.Ordinal) || config.scenario == "channel-echo-only")) || mesh.Channels.Any(c =>
                c.ChannelName == config.channelName && c.IsReady && c.ReadyTargetCount > 0));
        }
        else if (config.topology == "clientserver")
        {
            var channel = services.GetRequiredService<IZLinkClientServerRuntime>().GetStatus(config.channelName!);
            infrastructure &= channel.IsReady && channel.ReadyTargetCount > 0;
        }
        if (config.mode == "publish" && !config.source) infrastructure &= services.GetRequiredService<IZLinkFanoutRuntime>().GetStatus(config.channelName!).IsReady;
        var preparedObjects = measurement.ObjectPreparationEvidence;
        var needsPreparedObjects = config.objectRole != "None" && (config.source || config.objectRole == "Server");
        var objects = !needsPreparedObjects || preparedObjects is not null;
        var probe = measurement.SetupEvidence.Any(item => !ReferenceEquals(item, preparedObjects));
        List<object> evidence = [new { kind = "publicStatus", source = "public Framework runtime status", observedValue = PublicStatus(services) }];
        if (config.listenerEndpoint is not null) evidence.Add(new { kind = "verifiedListenerReservation",
            source = "role config; coordinator OS bind reservation and public host startup", observedValue = config.listenerEndpoint });
        evidence.AddRange(measurement.SetupEvidence);
        evidence.AddRange(measurement.ErrorEvidence);
        List<string> reasons = [];
        if (!infrastructure) reasons.Add("Public host/channel/listener infrastructure is not ready.");
        if (!objects) reasons.Add("Public object preparation has not completed.");
        if (!probe) reasons.Add("No successful typed probe echo has been observed.");
        if (measurement.HasErrors) reasons.Add("Application preparation or phase failed.");
        return new(config.runId, config.cellId, config.role, config.roleInstance, infrastructure,
            objects, probe, infrastructure && objects && probe && !measurement.HasErrors, PerfClock.UnixMs, evidence.ToArray(), reasons.ToArray());
    }
}

public sealed class RoutingIdObservationConverter : JsonConverter<RoutingId>
{
    public override RoutingId Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        throw new NotSupportedException("RoutingId observation is output-only.");
    public override void Write(Utf8JsonWriter writer, RoutingId value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToHex());
}
