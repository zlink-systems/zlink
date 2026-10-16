package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscription;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.BiFunction;

final class ZLinkSpotHandlerCatalog implements ZLinkSpotHandlerRegistry {
    private final String registrationClosedMessage;
    private final List<Class<?>> configuredHandlerTypes = new ArrayList<>();
    private final List<ConfiguredSubscription> configuredSubscriptions = new ArrayList<>();
    private boolean registrationOpen = true;
    private Map<String, SpotPacketHandlerRegistration> packetHandlers = Map.of();
    private Map<ZLinkBackendSubscription, List<SpotSubscriptionHandlerRegistration>>
            subscriptionHandlers = Map.of();

    ZLinkSpotHandlerCatalog(String registrationClosedMessage) {
        this.registrationClosedMessage = registrationClosedMessage;
    }

    @Override
    public void addHandler(Class<?> handlerType) {
        requireOpenHandler(handlerType);
        configuredHandlerTypes.add(handlerType);
    }

    @Override
    public void addSubscribe(Class<?> handlerType, String channelName, String topic) {
        requireOpenHandler(handlerType);
        configuredSubscriptions.add(
                new ConfiguredSubscription(
                        handlerType, ZLinkBackendSubscription.registration(channelName, topic)));
    }

    private void requireOpenHandler(Class<?> handlerType) {
        if (!registrationOpen) {
            throw new ZLinkConfigurationException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION, registrationClosedMessage);
        }
        if (handlerType == null) {
            throw new ZLinkConfigurationException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION, "SPOT handler type is required");
        }
    }

    void closeRegistration(
            BiFunction<List<Class<?>>, List<ConfiguredSubscription>, Registrations> loader) {
        registrationOpen = false;
        Registrations registrations =
                loader.apply(
                        List.copyOf(configuredHandlerTypes), List.copyOf(configuredSubscriptions));
        packetHandlers = registrations.packetHandlers();
        subscriptionHandlers = registrations.subscriptionHandlers();
    }

    SpotPacketHandlerRegistration packetHandler(String packetName) {
        return packetHandlers.get(packetName);
    }

    List<SpotSubscriptionHandlerRegistration> subscriptionHandlers(
            String channelName, String topic) {
        return subscriptionHandlers.getOrDefault(
                new ZLinkBackendSubscription(channelName, topic), List.of());
    }

    Set<ZLinkBackendSubscription> subscriptions() {
        return subscriptionHandlers.keySet();
    }

    record ConfiguredSubscription(Class<?> handlerType, ZLinkBackendSubscription subscription) {}

    record Registrations(
            Map<String, SpotPacketHandlerRegistration> packetHandlers,
            Map<ZLinkBackendSubscription, List<SpotSubscriptionHandlerRegistration>>
                    subscriptionHandlers) {

        Registrations {
            packetHandlers = Map.copyOf(packetHandlers);
            Map<ZLinkBackendSubscription, List<SpotSubscriptionHandlerRegistration>> subscriptions =
                    new HashMap<>();
            subscriptionHandlers.forEach(
                    (topic, handlers) -> subscriptions.put(topic, List.copyOf(handlers)));
            subscriptionHandlers = Map.copyOf(subscriptions);
        }
    }
}
