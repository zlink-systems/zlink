/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.core.AtomicCounter;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ZlinkStopwatch;
import systems.zlink.contracts.core.ZlinkThread;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.Socket;

import java.time.Duration;
import java.util.Objects;

/** Runtime factory wiring used only by the public {@code Zlink} facade. */
public final class ContractAccess {
    private static volatile RuntimeFactoryAccess runtimeFactoryAccess;

    private ContractAccess() {
    }

    public interface RuntimeFactoryAccess {
        Context createContext();

        AtomicCounter createAtomicCounter();

        ZlinkStopwatch createStopwatch();

        ZlinkThread createThread(Runnable task);

        Poller createPoller();

        ZlinkTimer createTimer();

        int errno();

        String strerror(int errnum);

        boolean has(String capability);

        int[] version();

        void proxy(Socket frontend, Socket backend, Socket capture);

        void proxySteerable(Socket frontend, Socket backend, Socket capture,
                            Socket control);

        void sleep(int seconds);

        void sleep(Duration duration);

        void multipartClose(Message[] parts);
    }

    public static void register(RuntimeFactoryAccess access) {
        runtimeFactoryAccess = Objects.requireNonNull(access, "access");
    }

    public static Context createContext() {
        return runtimeFactoryAccess().createContext();
    }

    public static AtomicCounter createAtomicCounter() {
        return runtimeFactoryAccess().createAtomicCounter();
    }

    public static ZlinkStopwatch createStopwatch() {
        return runtimeFactoryAccess().createStopwatch();
    }

    public static ZlinkThread createThread(Runnable task) {
        return runtimeFactoryAccess().createThread(task);
    }

    public static Poller createPoller() {
        return runtimeFactoryAccess().createPoller();
    }

    public static ZlinkTimer createTimer() {
        return runtimeFactoryAccess().createTimer();
    }

    public static int errno() {
        return runtimeFactoryAccess().errno();
    }

    public static String strerror(int errnum) {
        return runtimeFactoryAccess().strerror(errnum);
    }

    public static boolean has(String capability) {
        return runtimeFactoryAccess().has(capability);
    }

    public static int[] version() {
        return runtimeFactoryAccess().version();
    }

    public static void proxy(Socket frontend, Socket backend, Socket capture) {
        runtimeFactoryAccess().proxy(frontend, backend, capture);
    }

    public static void proxySteerable(Socket frontend, Socket backend,
                                      Socket capture, Socket control) {
        runtimeFactoryAccess().proxySteerable(frontend, backend, capture,
            control);
    }

    public static void sleep(int seconds) {
        runtimeFactoryAccess().sleep(seconds);
    }

    public static void sleep(Duration duration) {
        runtimeFactoryAccess().sleep(duration);
    }

    public static void multipartClose(Message[] parts) {
        runtimeFactoryAccess().multipartClose(parts);
    }

    private static RuntimeFactoryAccess runtimeFactoryAccess() {
        if (runtimeFactoryAccess == null) {
            load("systems.zlink.runtime.core.NativeRuntimeFactory");
        }
        if (runtimeFactoryAccess == null) {
            throw new IllegalStateException(
                "missing contract access for runtime factory");
        }
        return runtimeFactoryAccess;
    }

    private static void load(String className) {
        try {
            Class.forName(className, true, ContractAccess.class.getClassLoader());
        } catch (ClassNotFoundException ex) {
            throw new IllegalStateException("cannot load " + className, ex);
        }
    }
}
