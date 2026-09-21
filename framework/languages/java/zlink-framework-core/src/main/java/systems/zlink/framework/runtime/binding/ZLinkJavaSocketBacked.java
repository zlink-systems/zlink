package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

import java.time.Duration;

interface ZLinkJavaSocketBacked extends ZLinkBackendObject, ZLinkJavaAdmissionBacked {
    Socket nativeSocket();

    default Duration admissionTimeout() {
        Duration configured = nativeSocket().options().sendTimeout();
        return configured.isNegative() || configured.isZero() ? Duration.ofSeconds(1) : configured;
    }
}
