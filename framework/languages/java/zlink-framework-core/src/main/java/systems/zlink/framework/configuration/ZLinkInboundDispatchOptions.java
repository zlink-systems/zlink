package systems.zlink.framework.configuration;

import java.util.OptionalLong;

/**
 * Host-wide inbound Application byte budget. Leaving
 * {@code applicationHwmBytes} unset selects Auto sizing, {@code 0} disables the
 * limit, and a positive value applies that exact host-wide byte limit.
 */
public interface ZLinkInboundDispatchOptions {
    OptionalLong applicationHwmBytes();

    void setApplicationHwmBytes(long value);

    ZLinkApplicationHwmProfile applicationHwmProfile();

    void setApplicationHwmProfile(ZLinkApplicationHwmProfile value);

    OptionalLong processMemoryLimitBytes();

    void setProcessMemoryLimitBytes(long value);
}
