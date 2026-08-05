package systems.zlink.framework.runtime.handlers;

import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ZLinkHandlerScanValidation {
    private ZLinkHandlerScanValidation() {
    }

    static String requireTopic(Class<?> handlerType, String topic) {
        if (topic == null || topic.isBlank()) {
            throw new ZLinkConfigurationException(
                "SPOT subscription handler topic is required: " + handlerType.getName());
        }
        return topic;
    }
}
