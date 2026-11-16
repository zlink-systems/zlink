using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotStartupValidator
{
    public static ValueTask ValidateAsync(
        IServiceProvider services,
        ZLinkFrameworkRegistration registration
    )
    {
        foreach (var handler in registration.ScannedHandlerCatalog.SpotHandlers)
        {
            var ownerTypes = registration
                .SpotNodes.Values.Where(node =>
                    handler.SpotNodeName is null || handler.SpotNodeName == node.SpotNodeName
                )
                .SelectMany(node =>
                    node.SpotFactories.Concat(
                            node.InstanceSpotFactories.Values.Select(factory => factory.SpotType)
                        )
                        .Concat(
                            node.EntrySpotType is { } entry ? new[] { entry } : Array.Empty<Type>()
                        )
                );
            var expectedOwner =
                ownerTypes.FirstOrDefault(type => type == handler.SpotType)
                ?? ownerTypes.FirstOrDefault(handler.SpotType.IsAssignableFrom);
            if (expectedOwner is not null)
                ZLinkHandlerContractDescriptorSupport.RequireExactType(
                    handler.HandlerType,
                    expectedOwner,
                    handler.Kind
                        is ZLinkScannedSpotHandlerKind.Packet
                            or ZLinkScannedSpotHandlerKind.Subscription
                        ? handler.Method?.DeclaringType ?? handler.SpotType
                        : handler.SpotType,
                    "SPOT handler"
                );
        }
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
                if (handler.SpotNodeName is not null && handler.SpotNodeName != node.SpotNodeName)
                    continue;
                if (!spotTypes.Contains(handler.SpotType))
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
