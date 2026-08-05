/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.AtomicCounter;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.ZlinkStopwatch;
import systems.zlink.contracts.core.ZlinkThread;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.runtime.nativeapi.ContractAccess;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.runtime.eventing.NativePoller;
import systems.zlink.runtime.eventing.NativeTimer;
import java.time.Duration;

final class NativeRuntimeFactory {
    static {
        ContractAccess.register(new ContractAccess.RuntimeFactoryAccess() {
            @Override
            public Context createContext() {
                return new NativeContext();
            }

            @Override
            public AtomicCounter createAtomicCounter() {
                return NativeCoreResources.createAtomicCounter();
            }

            @Override
            public ZlinkStopwatch createStopwatch() {
                return NativeCoreResources.createStopwatch();
            }

            @Override
            public ZlinkThread createThread(Runnable task) {
                return NativeCoreResources.createThread(task);
            }

            @Override
            public Poller createPoller() {
                return NativePoller.create();
            }

            @Override
            public ZlinkTimer createTimer() {
                return NativeTimer.create();
            }

            @Override
            public int errno() {
                return NativeCoreRuntime.errno();
            }

            @Override
            public String strerror(int errnum) {
                return NativeCoreRuntime.strerror(errnum);
            }

            @Override
            public boolean has(String capability) {
                return NativeCoreRuntime.has(capability);
            }

            @Override
            public int[] version() {
                return NativeCoreRuntime.version();
            }

            @Override
            public void proxy(Socket frontend, Socket backend, Socket capture) {
                NativeCoreRuntime.proxy(frontend, backend, capture);
            }

            @Override
            public void proxySteerable(Socket frontend, Socket backend,
                                       Socket capture, Socket control) {
                NativeCoreRuntime.proxySteerable(frontend, backend, capture,
                    control);
            }

            @Override
            public void sleep(int seconds) {
                NativeCoreRuntime.sleep(seconds);
            }

            @Override
            public void sleep(Duration duration) {
                NativeCoreRuntime.sleep(duration);
            }

            @Override
            public void multipartClose(Message[] parts) {
                NativeCoreRuntime.multipartClose(parts);
            }
        });
    }

    private NativeRuntimeFactory() {
    }
}
