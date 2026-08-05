using Microsoft.Extensions.Configuration;

using RuntimeMonitoring.Server.Service.Handlers;
using RuntimeMonitoring.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace RuntimeMonitoring.Server.Service.Support;

internal sealed class ChannelMonitoringRoleHost
{
    private readonly WebApplicationBuilder _builder;
    private readonly ServerOptions _options;

    private ChannelMonitoringRoleHost(string[] args, string defaultRole)
    {
        _options = ServerOptions.Parse(args, defaultRole);
        _builder = WebApplication.CreateBuilder(args);
        _builder.Configuration.Sources.Clear();
        _builder.Configuration.AddInMemoryCollection();
        _builder.WebHost.UseUrls(_options.HttpUrl);
        _builder.Services.AddSingleton(new EvidenceStore(_options.EvidenceFile, _options.Rid));
        _builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            var mesh = framework.AddRouteMesh(RuntimeMonitoringNames.Channel)
                .Listen(Require(_options.ChannelEndpoint, "ChannelEndpoint"))
                .SetRoutingId(RoutingId.From(_options.Rid));
            mesh.Channel(RuntimeMonitoringNames.Channel).Server()
                .AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq");
            mesh.PeerConnections.Connect(Require(_options.ChannelEndpoint, "ChannelEndpoint"));
            _builder.Services.AddSingleton(mesh.PeerConnections);
        });
    }

    public static ChannelMonitoringRoleHost Create(string[] args, string defaultRole)
    {
        return new ChannelMonitoringRoleHost(args, defaultRole);
    }

    public WebApplication Build()
    {
        var app = _builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", _options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected => entries.Skip(request.AfterIndex)
                               .Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
                           && request.ContainsAnyGroups.All(group => group.Any(expected =>
                               entries.Skip(request.AfterIndex)
                                   .Any(entry => entry.Contains(expected, StringComparison.Ordinal)))),
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken);
            return Results.Ok(snapshot.Skip(request.AfterIndex).ToArray());
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        app.MapPost("/profile/request", async (
            ProfileReq request,
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var response = await channel.RequestToChannel(RuntimeMonitoringNames.Channel, request)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async<ProfileRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapPost("/admin/connect", (IZLinkMeshPeerConnections connections) =>
        {
            connections.Connect(Require(_options.ChannelEndpoint, "ChannelEndpoint"));
            return Results.Ok(new { status = "connected" });
        });
        app.MapPost("/admin/disconnect", (IZLinkMeshPeerConnections connections) =>
        {
            connections.Disconnect(Require(_options.ChannelEndpoint, "ChannelEndpoint"));
            return Results.Ok(new { status = "disconnected" });
        });
        return app;
    }

    private static string Require(string? value, string name)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{name} is required.")
            : value;
    }
}
