package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

interface ZLinkJavaSocketBacked
    extends ZLinkBackendObject, ZLinkJavaAdmissionBacked {
    Socket nativeSocket();

    default Duration admissionTimeout() {
        Duration configured = nativeSocket().options().sendTimeout();
        return configured.isNegative() || configured.isZero()
            ? Duration.ofSeconds(1)
            : configured;
    }

}
