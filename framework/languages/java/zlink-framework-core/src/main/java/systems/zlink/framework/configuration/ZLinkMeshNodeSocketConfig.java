package systems.zlink.framework.configuration;

import java.time.Duration;
import java.util.Optional;

public interface ZLinkMeshNodeSocketConfig {
    long sendHighWaterMark();

    void setSendHighWaterMark(long value);

    long receiveHighWaterMark();

    void setReceiveHighWaterMark(long value);

    Optional<Duration> receiveTimeout();

    void setReceiveTimeout(Duration value);
}
