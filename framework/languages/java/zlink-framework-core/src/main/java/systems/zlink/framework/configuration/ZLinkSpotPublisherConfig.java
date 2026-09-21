package systems.zlink.framework.configuration;

import org.jspecify.annotations.Nullable;

import java.time.Duration;
import java.util.Optional;

public interface ZLinkSpotPublisherConfig {
    int sendHighWaterMark();

    void setSendHighWaterMark(int value);

    Optional<Duration> sendTimeout();

    /** Sets the send timeout, or clears it to the one-second default when null. */
    void setSendTimeout(@Nullable Duration value);

    Optional<Duration> linger();

    void setLinger(Duration value);
}
