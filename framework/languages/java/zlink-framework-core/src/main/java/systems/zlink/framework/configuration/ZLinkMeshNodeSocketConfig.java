package systems.zlink.framework.configuration;

import java.time.Duration;
import java.util.Optional;
import org.jspecify.annotations.Nullable;

public interface ZLinkMeshNodeSocketConfig {
    long maxMessageSize();

    void setMaxMessageSize(long value);

    long sendHighWaterMark();

    void setSendHighWaterMark(long value);

    long receiveHighWaterMark();

    void setReceiveHighWaterMark(long value);

    long mailboxMessageBudget();

    void setMailboxMessageBudget(long value);

    long mailboxByteBudget();

    void setMailboxByteBudget(long value);

    Optional<Duration> receiveTimeout();

    void setReceiveTimeout(Duration value);

    Optional<Duration> sendTimeout();

    /** Sets the send timeout, or clears it to the one-second default when null. */
    void setSendTimeout(@Nullable Duration value);
}
