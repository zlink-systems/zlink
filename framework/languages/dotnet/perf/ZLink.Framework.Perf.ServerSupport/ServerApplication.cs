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
        if (config.objectRole != "None" || config.store is not null || config.spotIds.Length != 0 || config.actorIds.Length != 0)
            throw new ArgumentException("Phase 1 baselines register no object role or Store.");
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
            configure(options);
        });
        return builder;
    }
    public static void Map(WebApplication app, Func<Task>? workload = null)
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
        if (config.topology == "routemesh") return new { host, routeMesh = services.GetRequiredService<IZLinkRouteMeshRuntime>().GetStatus(config.meshName!) };
        if (config.topology == "clientserver") return new { host, clientServer = services.GetRequiredService<IZLinkClientServerRuntime>().GetStatus(config.channelName!) };
        return new { host };
    }
    public static PerfReady Ready(IServiceProvider services)
    {
        var config = services.GetRequiredService<RoleConfig>();
        var measurement = services.GetRequiredService<Measurement>();
        var host = services.GetRequiredService<IZLinkFrameworkRuntime>().Status;
        var infrastructure = host.IsReady;
        if (config.topology == "routemesh")
        {
            var mesh = services.GetRequiredService<IZLinkRouteMeshRuntime>().GetStatus(config.meshName!);
            // Channel messaging §3: RouteMesh excludes the sending node itself from candidates.
            // Only the source needs a selectable remote target; the receiver proves dispatch by echo.
            infrastructure &= mesh.IsReady && (!config.source || mesh.Channels.Any(c =>
                c.ChannelName == config.channelName && c.IsReady && c.ReadyTargetCount > 0));
        }
        else if (config.topology == "clientserver")
        {
            var channel = services.GetRequiredService<IZLinkClientServerRuntime>().GetStatus(config.channelName!);
            infrastructure &= channel.IsReady && channel.ReadyTargetCount > 0;
        }
        var probe = measurement.SetupEvidence.Length > 0;
        List<object> evidence = [new { kind = "publicStatus", source = "public Framework runtime status", observedValue = PublicStatus(services) }];
        if (config.listenerEndpoint is not null) evidence.Add(new { kind = "verifiedListenerReservation",
            source = "role config; coordinator OS bind reservation and public host startup", observedValue = config.listenerEndpoint });
        evidence.AddRange(measurement.SetupEvidence);
        evidence.AddRange(measurement.ErrorEvidence);
        List<string> reasons = [];
        if (!infrastructure) reasons.Add("Public host/channel/listener infrastructure is not ready.");
        if (!probe) reasons.Add("No successful typed probe echo has been observed.");
        if (measurement.HasErrors) reasons.Add("Application preparation or phase failed.");
        return new(config.runId, config.cellId, config.role, config.roleInstance, infrastructure,
            true, probe, infrastructure && probe && !measurement.HasErrors, PerfClock.UnixMs, evidence.ToArray(), reasons.ToArray());
    }
}

public sealed class RoutingIdObservationConverter : JsonConverter<RoutingId>
{
    public override RoutingId Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        throw new NotSupportedException("RoutingId observation is output-only.");
    public override void Write(Utf8JsonWriter writer, RoutingId value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.ToHex());
}
