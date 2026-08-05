package systems.zlink.framework.runtime.configuration;

import java.util.Objects;
import java.util.OptionalLong;
import systems.zlink.framework.configuration.ZLinkApplicationHwmProfile;
import systems.zlink.framework.configuration.ZLinkInboundDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;

/**
 * Host-wide inbound Application byte budget. Mirrors the .NET
 * {@code ZLinkInboundDispatchOptionsModel}, including the profile and process
 * memory limit validation.
 */
public final class ZLinkInboundDispatchRegistration
    implements ZLinkInboundDispatchOptions {
    private Long applicationHwmBytes;
    private ZLinkApplicationHwmProfile applicationHwmProfile =
        ZLinkApplicationHwmProfile.BALANCED;
    private Long processMemoryLimitBytes;

    @Override
    public OptionalLong applicationHwmBytes() {
        return applicationHwmBytes == null
            ? OptionalLong.empty()
            : OptionalLong.of(applicationHwmBytes);
    }

    @Override
    public void setApplicationHwmBytes(long value) {
        if (value < 0) {
            throw new ZLinkConfigurationException(
                "Application HWM bytes must be zero or positive.");
        }
        applicationHwmBytes = value;
    }

    @Override
    public ZLinkApplicationHwmProfile applicationHwmProfile() {
        return applicationHwmProfile;
    }

    @Override
    public void setApplicationHwmProfile(ZLinkApplicationHwmProfile value) {
        applicationHwmProfile =
            Objects.requireNonNull(value, "applicationHwmProfile");
    }

    @Override
    public OptionalLong processMemoryLimitBytes() {
        return processMemoryLimitBytes == null
            ? OptionalLong.empty()
            : OptionalLong.of(processMemoryLimitBytes);
    }

    @Override
    public void setProcessMemoryLimitBytes(long value) {
        if (value <= 0) {
            throw new ZLinkConfigurationException(
                "ProcessMemoryLimitBytes must be a positive finite byte value.");
        }
        processMemoryLimitBytes = value;
    }
}
