package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

/** The Channel scope and topic of one logical Spot subscription. */
public record ZLinkBackendSubscription(String channelName, String topic) {
    public static ZLinkBackendSubscription registration(String channelName, String topic) {
        if (channelName == null || channelName.isBlank()) {
            throw new ZLinkConfigurationException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "SPOT subscription channel name is required");
        }
        if (topic == null || topic.isBlank()) {
            throw new ZLinkConfigurationException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "SPOT subscription topic is required");
        }
        return new ZLinkBackendSubscription(channelName, topic);
    }
}
