package systems.zlink.framework.runtime.internal.channels;

import java.time.Duration;

/** Shared channel family send-timeout default for socket setup and admission. */
public final class ZLinkChannelAdmissionTimeout {
    public static final Duration DEFAULT_SEND_TIMEOUT = Duration.ofSeconds(1);

    private ZLinkChannelAdmissionTimeout() {}
}
