package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.function.Consumer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

interface ZLinkJavaAdmissionBacked {
    default ZLinkBackendObject admissionSource() {
        return (ZLinkBackendObject) this;
    }

    default void setAdmissionReadyHandler(
        Consumer<ZLinkBackendAdmissionKey> handler) {
    }

    default void setAdmissionShutdownHandler(Runnable handler) {
    }

    default Duration admissionTimeout() {
        return Duration.ofSeconds(1);
    }

    default int admissionPendingCapacity() {
        return 4096;
    }
}
