package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.function.Consumer;
import java.util.Collections;
import java.util.Map;
import java.util.WeakHashMap;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

interface ZLinkJavaSocketBacked
    extends ZLinkBackendObject, ZLinkJavaAdmissionBacked {
    Map<Socket, Runnable> ADMISSION_SHUTDOWN_HANDLERS =
        Collections.synchronizedMap(new WeakHashMap<>());

    Socket nativeSocket();

    @Override
    default ZLinkBackendObject admissionSource() {
        return this;
    }

    default void setAdmissionReadyHandler(
        Consumer<ZLinkBackendAdmissionKey> handler) {
        nativeSocket().setSendReadyHandler(
            () -> handler.accept(ZLinkBackendAdmissionKey.socket()));
    }

    default Duration admissionTimeout() {
        Duration configured = nativeSocket().options().sendTimeout();
        return configured.isNegative() || configured.isZero()
            ? Duration.ofSeconds(1)
            : configured;
    }

    default void setAdmissionShutdownHandler(Runnable handler) {
        ADMISSION_SHUTDOWN_HANDLERS.put(nativeSocket(), handler);
    }

    @Override
    default int admissionPendingCapacity() {
        return 4096;
    }

    default void notifyAdmissionShutdown() {
        Runnable handler = ADMISSION_SHUTDOWN_HANDLERS.remove(nativeSocket());
        if (handler != null) {
            handler.run();
        }
    }
}
