package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ConfiguredSocketRuntimeOptions implements ZLinkSocketRuntimeOptions {
    private long maxMessageSize = 16_777_216L;
    private int weight = 100;

    @Override
    public long maxMessageSize() {
        return maxMessageSize;
    }

    @Override
    public void maxMessageSize(long value) {
        if (value < 0) {
            throw new ZLinkConfigurationException(
                "MaxMessageSize must be zero or a positive byte count.");
        }
        maxMessageSize = value;
    }

    @Override
    public int weight() {
        return weight;
    }

    @Override
    public void weight(int value) {
        ZLinkChannelRuntime.validatePeerWeight(value);
        weight = value;
    }
}
