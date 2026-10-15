using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotStartupValidator
{
    public static ValueTask ValidateAsync(
        IServiceProvider services,
        ZLinkFrameworkRegistration registration
    )
    {
        foreach (var node in registration.SpotNodes.Values)
        {
            ZLinkScopedHandlerInstanceOwner.Validate(
                services,
                node.SpotFactories,
                typeof(IZLinkSpotContext)
            );
            if (node.EntrySpotType is { } entry)
                ZLinkScopedHandlerInstanceOwner.Validate(
                    services,
                    [entry],
                    typeof(IZLinkEntrySpotContext)
                );
            ZLinkScopedHandlerInstanceOwner.Validate(
                services,
                node.InstanceSpotFactories.Values.Select(static factory => factory.SpotType),
                typeof(IZLinkInstanceSpotContext)
            );
            var spotTypes = node
                .SpotFactories.Concat(
                    node.InstanceSpotFactories.Values.Select(static factory => factory.SpotType)
                )
                .ToHashSet();
            if (node.EntrySpotType is not null)
                spotTypes.Add(node.EntrySpotType);
            foreach (var handler in registration.ScannedHandlerCatalog.SpotHandlers)
            {
                if (
                    !spotTypes.Contains(handler.SpotType)
                    || (
                        handler.SpotNodeName is not null
                        && handler.SpotNodeName != node.SpotNodeName
                    )
                )
                    continue;
                if (handler.Kind == ZLinkScannedSpotHandlerKind.Timer)
                    ZLinkSpotTimerRegistry.ValidateRegistration(
                        handler.TimerName ?? string.Empty,
                        handler.TimerPeriod,
                        null
                    );
                if (handler.HandlerType != handler.SpotType)
                    ZLinkScopedHandlerInstanceOwner.Validate(services, [handler.HandlerType]);
            }
            ZLinkScopedHandlerInstanceOwner.Validate(
                services,
                node.RouteSendHandlers.Concat(node.RouteRequestHandlers)
                    .Select(static handler => handler.HandlerType)
            );
        }
        return ValueTask.CompletedTask;
    }
}
