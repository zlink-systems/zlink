package systems.zlink.framework.runtime.handlers;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;

public final class ZLinkHandlerScanValidation {
    private ZLinkHandlerScanValidation() {}

    public static String requireTopic(Class<?> handlerType, ZLinkSpotSubscription subscription) {
        return requireTopic(handlerType, subscription == null ? null : subscription.topic());
    }

    public static String requireTopic(Class<?> handlerType, String topic) {
        if (topic == null || topic.isBlank()) {
            throw new ZLinkConfigurationException(
                    "SPOT subscription handler topic is required: " + handlerType.getName());
        }
        return topic;
    }
}
