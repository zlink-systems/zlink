package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Function;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;

final class ZLinkSpotHandlerCatalog implements ZLinkSpotHandlerRegistry {
    private final String registrationClosedMessage;
    private final List<Class<?>> configuredHandlerTypes = new ArrayList<>();
    private boolean registrationOpen = true;
    private Map<String, SpotPacketHandlerRegistration> packetHandlers = Map.of();
    private Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptionHandlers = Map.of();

    ZLinkSpotHandlerCatalog(String registrationClosedMessage) {
        this.registrationClosedMessage = registrationClosedMessage;
    }

    @Override
    public void addHandler(Class<?> handlerType) {
        if (!registrationOpen) {
            throw new ZLinkConfigurationException(registrationClosedMessage);
        }
        if (handlerType == null) {
            throw new ZLinkConfigurationException("SPOT handler type is required");
        }
        configuredHandlerTypes.add(handlerType);
    }

    void closeRegistration(Function<List<Class<?>>, Registrations> loader) {
        registrationOpen = false;
        Registrations registrations = loader.apply(List.copyOf(configuredHandlerTypes));
        packetHandlers = registrations.packetHandlers();
        subscriptionHandlers = registrations.subscriptionHandlers();
    }

    SpotPacketHandlerRegistration packetHandler(String packetName) {
        return packetHandlers.get(packetName);
    }

    List<SpotSubscriptionHandlerRegistration> subscriptionHandlers(String topic) {
        return subscriptionHandlers.getOrDefault(topic, List.of());
    }

    Set<String> subscriptionTopics() {
        return subscriptionHandlers.keySet();
    }

    record Registrations(
        Map<String, SpotPacketHandlerRegistration> packetHandlers,
        Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptionHandlers) {

        Registrations {
            packetHandlers = Map.copyOf(packetHandlers);
            Map<String, List<SpotSubscriptionHandlerRegistration>> subscriptions = new HashMap<>();
            subscriptionHandlers.forEach(
                (topic, handlers) -> subscriptions.put(topic, List.copyOf(handlers)));
            subscriptionHandlers = Map.copyOf(subscriptions);
        }
    }
}
