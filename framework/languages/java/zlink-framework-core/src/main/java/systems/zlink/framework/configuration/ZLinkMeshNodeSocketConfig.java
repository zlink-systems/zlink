package systems.zlink.framework.configuration;

import org.jspecify.annotations.Nullable;

import java.time.Duration;
import java.util.Optional;

public interface ZLinkMeshNodeSocketConfig {
    long sendHighWaterMark();

    void setSendHighWaterMark(long value);

    long receiveHighWaterMark();

    void setReceiveHighWaterMark(long value);

    Optional<Duration> receiveTimeout();

    void setReceiveTimeout(Duration value);

    Optional<Duration> sendTimeout();

    /** Sets the send timeout, or clears it to the one-second default when null. */
    void setSendTimeout(@Nullable Duration value);
}
