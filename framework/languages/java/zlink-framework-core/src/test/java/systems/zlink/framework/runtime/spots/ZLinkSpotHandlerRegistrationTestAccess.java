package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;

import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;

/** Test seam for Kotlin coverage of the Java-owned Spot handler registration pipeline. */
public final class ZLinkSpotHandlerRegistrationTestAccess {
    private ZLinkSpotHandlerRegistrationTestAccess() {}

    public static Object load(
            Class<?> spotType, List<Class<?>> handlerTypes, List<?> configuredSubscriptions) {
        ZLinkScannedHandlerCatalog scannedHandlers = new ZLinkScannedHandlerCatalog(List.of());
        ZLinkSpotActorHandlerCatalog actorHandlers =
                new ZLinkSpotActorHandlerCatalog(scannedHandlers, null);
        ZLinkSpotHandlerLoader loader =
                new ZLinkSpotHandlerLoader(scannedHandlers, actorHandlers, handlerType -> null);
        return loader.load(
                spotType,
                handlerTypes,
                (List<ZLinkSpotHandlerCatalog.ConfiguredSubscription>) configuredSubscriptions,
                (name, period, handlerType, options) -> CompletableFuture.completedFuture(null));
    }

    public static Set<?> loadSubscriptions(
            Class<?> spotType, Class<?> handlerType, String channelName, String topic) {
        ZLinkSpotHandlerCatalog catalog = new ZLinkSpotHandlerCatalog("closed");
        ZLinkSpotHandlerRegistry registry = catalog;
        registry.addSubscribe(handlerType, channelName, topic);
        catalog.closeRegistration(
                (handlerTypes, subscriptions) ->
                        (ZLinkSpotHandlerCatalog.Registrations)
                                load(spotType, handlerTypes, subscriptions));
        return catalog.subscriptions();
    }
}
