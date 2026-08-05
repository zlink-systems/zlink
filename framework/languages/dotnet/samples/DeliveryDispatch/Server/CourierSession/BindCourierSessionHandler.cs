using DeliveryDispatch.Shared.Contracts;
using Zlink.Framework.Contracts.Streams;

namespace DeliveryDispatch.Server.CourierSession;

internal sealed class BindCourierSessionHandler(
    CourierSessionBinder binder)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, BindCourierSessionReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        BindCourierSessionReq request,
        CancellationToken cancellationToken)
    {
        var bound = await binder.BindAsync(request.CourierId, context, cancellationToken);
        await context.Client.Reply(bound).Async(cancellationToken);
    }
}
