using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Streams;

namespace DeliveryDispatch.Server.CourierSession;

internal sealed class CourierSessionBinder(
    IZLinkActorManager actors,
    ILogger<CourierSessionBinder> logger)
{
    public async ValueTask<BindCourierSessionRes> BindAsync(
        string courierId,
        IZLinkSessionContext context,
        CancellationToken cancellationToken)
    {
        var actor = await FindOrEnsureActorAsync(courierId, cancellationToken);
        logger.LogInformation(
            "deliverydispatch courier-session: actor ready courier={CourierId}",
            courierId);
        await context.Actors.BindOrGetAsync(
            actor,
            cancellationToken);

        logger.LogInformation(
            "deliverydispatch courier-session: bound courier={CourierId} session={SessionId}",
            courierId,
            context.SessionId);

        return new BindCourierSessionRes(courierId);
    }

    private async ValueTask<ActorRef> FindOrEnsureActorAsync(
        string courierId,
        CancellationToken cancellationToken)
    {
        var result = await actors
            .GetOrCreate(courierId, SampleNames.CourierActorType)
            .InMesh(SampleNames.CourierMeshName)
            .Request(new EnsureCourierActorReq(courierId))
            .Async(cancellationToken);
        var actor = result switch
        {
            ZLinkActorCreateResult.Existing existing => existing.Actor,
            ZLinkActorCreateResult.Created created => created.Actor,
            ZLinkActorCreateResult.Rejected => throw new InvalidOperationException(
                $"Courier actor '{courierId}' creation was rejected."),
            _ => throw new InvalidOperationException(
                $"Courier actor '{courierId}' returned an unknown creation result.")
        };
        return actor;
    }
}
