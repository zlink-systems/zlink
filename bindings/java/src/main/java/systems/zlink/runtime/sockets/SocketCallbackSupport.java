/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodType;
import java.util.concurrent.ExecutorService;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.NativeCallbackSupport;
import systems.zlink.runtime.nativeapi.RuntimeResources;

final class SocketCallbackSupport implements AutoCloseable {
    private static final Linker LINKER = Linker.nativeLinker();

    private final SocketCore owner;
    private final NativeCallbackSupport callbacks =
        new NativeCallbackSupport("zlink-socket-callback");

    SocketCallbackSupport(SocketCore owner) {
        this.owner = owner;
    }

    ExecutorService executor() {
        return callbacks.executor();
    }

    Arena install(String callbackName, MethodType callbackType,
                  FunctionDescriptor descriptor, String nativeOperation,
                  CallbackRegistration registration) {
        owner.ensureOpen();
        callbacks.ensureNoFailure();
        NativeCallbackSupport.ExecutorLease lease = callbacks.ensureExecutor();
        Arena arena = Arena.ofShared();
        MemorySegment stub = LINKER.upcallStub(
            owner.callbackHandle(callbackName, callbackType), descriptor,
            arena);
        boolean success = false;
        try {
            int rc = registration.register(stub);
            if (rc != 0) {
                throw ZlinkException.fromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.HANDLER);
            }
            success = true;
            return arena;
        } finally {
            if (!success) {
                callbacks.clearExecutorIfCreated(lease);
                RuntimeResources.closeArena(arena);
            }
        }
    }

    void recordFailure(RuntimeException failure) {
        callbacks.recordFailure(failure);
    }

    void ensureNoFailure() {
        callbacks.ensureNoFailure();
    }

    @Override
    public void close() {
        callbacks.close();
    }

    @FunctionalInterface
    interface CallbackRegistration {
        int register(MemorySegment stub);
    }
}
