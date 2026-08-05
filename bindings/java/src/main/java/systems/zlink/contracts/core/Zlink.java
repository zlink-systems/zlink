/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.runtime.nativeapi.ContractAccess;
import systems.zlink.contracts.sockets.Socket;
import java.time.Duration;

/**
 * Library entry point: factories for contexts, timers, pollers, and threads,
 * plus process-wide utilities (version, capabilities, proxying).
 */
public final class Zlink {
    private Zlink() {}

    public static Context createContext() {
        return ContractAccess.createContext();
    }

    public static AtomicCounter createAtomicCounter() {
        return ContractAccess.createAtomicCounter();
    }

    public static ZlinkStopwatch createStopwatch() {
        return ContractAccess.createStopwatch();
    }

    public static ZlinkThread createThread(Runnable task) {
        return ContractAccess.createThread(task);
    }

    public static Poller createPoller() {
        return ContractAccess.createPoller();
    }

    public static ZlinkTimer createTimer() {
        return ContractAccess.createTimer();
    }

    static int errno() {
        return ContractAccess.errno();
    }

    public static String strerror(int errnum) {
        return ContractAccess.strerror(errnum);
    }

    public static boolean has(String capability) {
        return ContractAccess.has(capability);
    }

    public static int[] version() {
        return ContractAccess.version();
    }

    public static void proxy(Socket frontend, Socket backend, Socket capture) {
        ContractAccess.proxy(frontend, backend, capture);
    }

    public static void proxySteerable(Socket frontend, Socket backend,
                                      Socket capture, Socket control) {
        ContractAccess.proxySteerable(frontend, backend, capture, control);
    }

    static void sleep(int seconds) {
        ContractAccess.sleep(seconds);
    }

    public static void sleep(Duration duration) {
        ContractAccess.sleep(duration);
    }

    static void multipartClose(Message[] parts) {
        ContractAccess.multipartClose(parts);
    }
}
