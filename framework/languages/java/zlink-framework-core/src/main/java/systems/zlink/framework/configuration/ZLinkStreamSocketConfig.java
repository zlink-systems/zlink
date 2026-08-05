package systems.zlink.framework.configuration;

/**
 * Socket limits used by one Framework STREAM listener.
 *
 * <p>A value of zero removes the separate STREAM message-size limit. When a
 * process-wide application HWM is enabled, validation requires a finite
 * positive value so that one received frame has a bounded size.</p>
 */
public interface ZLinkStreamSocketConfig {
    long maxMessageSize();

    void setMaxMessageSize(long value);
}
