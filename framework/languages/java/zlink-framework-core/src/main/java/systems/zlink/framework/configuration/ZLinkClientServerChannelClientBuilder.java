package systems.zlink.framework.configuration;

import org.jspecify.annotations.Nullable;

import java.time.Duration;

public interface ZLinkClientServerChannelClientBuilder {
    ZLinkClientServerChannelClientBuilder connect(String endpoint);

    /** Sets this channel client's send timeout, or restores the one-second default when null. */
    ZLinkClientServerChannelClientBuilder setSendTimeout(@Nullable Duration value);
}
