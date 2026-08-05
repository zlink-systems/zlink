using Microsoft.Extensions.DependencyInjection;
using SpotService.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Endpoints;

internal static class B10Endpoints
{
    public static void MapManualEndpoints(WebApplication app)
    {
        app.MapGet("/b10/manual/status", (IServiceProvider services) =>
        {
            var actorManagerProvided = services.GetService<IZLinkActorManager>() is not null;
            var spotManagerProvided = services.GetService<IZLinkSpotManager>() is not null;
            return Results.Ok(new B10ManualStatusRes(
                actorManagerProvided,
                spotManagerProvided,
                actorManagerProvided || spotManagerProvided));
        });
    }
}
