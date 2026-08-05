package systems.zlink.framework.configuration;

import java.time.Duration;
import java.util.Optional;
import org.jspecify.annotations.Nullable;

public interface ZLinkSpotPublisherConfig {
    int sendHighWaterMark();

    void setSendHighWaterMark(int value);

    Optional<Duration> sendTimeout();

    /** Sets the send timeout, or clears it to the one-second default when null. */
    void setSendTimeout(@Nullable Duration value);

    Optional<Duration> linger();

    void setLinger(Duration value);
}
