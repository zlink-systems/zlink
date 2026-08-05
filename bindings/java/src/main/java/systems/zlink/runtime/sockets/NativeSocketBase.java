/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptionKey;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.lang.foreign.MemorySegment;
import java.util.Objects;

/**
 * Common contract facade for zlink typed socket resources.
 */
abstract class NativeSocketBase implements Socket {
    private final CommonSocketOptions options;
    private final NativeSocketRuntime runtime;

    static {
        InternalAccess.register(new InternalAccess.SocketAccess() {
            @Override
            public MemorySegment handle(Socket socket) {
                return nativeSocket(socket).nativeHandle();
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<Integer> option,
                                  int value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<Long> option,
                                  long value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<String> option,
                                  String value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<byte[]> option,
                                  byte[] value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public <T> T getOption(Socket socket, SocketOptionKey<T> option) {
                return nativeSocket(socket).runtime.getOption(option);
            }

            @Override
            public void setDealerIntOption(Socket socket, int option,
                                           int value) {
                nativeSocket(socket).runtime.setDealerIntOption(option, value);
            }

            @Override
            public int getDealerIntOption(Socket socket, int option) {
                return nativeSocket(socket).runtime.getDealerIntOption(option);
            }

            @Override
            public int getRouterIntOption(Socket socket, int option) {
                return nativeSocket(socket).runtime.getRouterIntOption(option);
            }

            @Override
            public void setRouterIntOption(Socket socket, int option,
                                           int value) {
                nativeSocket(socket).runtime.setRouterIntOption(option, value);
            }

            @Override
            public boolean inCallback() {
                return NativeSocketRuntime.inCallbackContext();
            }

            @Override
            public void enterCallback() {
                NativeSocketRuntime.enterCallbackContext();
            }

            @Override
            public void leaveCallback() {
                NativeSocketRuntime.leaveCallbackContext();
            }

            private NativeSocketBase nativeSocket(Socket socket) {
                if (socket instanceof NativeSocketBase nativeSocket)
                    return nativeSocket;
                throw new IllegalArgumentException(
                    "socket is not backed by the native zlink runtime");
            }
        });
        ContractAccess.register(new ContractAccess.SocketOptionsAccess() {
            @Override
            public void setOption(Socket socket, SocketOptionKey<Integer> option,
                                  int value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<Long> option,
                                  long value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<String> option,
                                  String value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public void setOption(Socket socket, SocketOptionKey<byte[]> option,
                                  byte[] value) {
                nativeSocket(socket).runtime.setOption(option, value);
            }

            @Override
            public <T> T getOption(Socket socket, SocketOptionKey<T> option) {
                return nativeSocket(socket).runtime.getOption(option);
            }

            @Override
            public void setDealerIntOption(Socket socket, int option,
                                           int value) {
                nativeSocket(socket).runtime.setDealerIntOption(option, value);
            }

            @Override
            public int getDealerIntOption(Socket socket, int option) {
                return nativeSocket(socket).runtime.getDealerIntOption(option);
            }

            @Override
            public int getRouterIntOption(Socket socket, int option) {
                return nativeSocket(socket).runtime.getRouterIntOption(option);
            }

            @Override
            public void setRouterIntOption(Socket socket, int option,
                                           int value) {
                nativeSocket(socket).runtime.setRouterIntOption(option, value);
            }

            private NativeSocketBase nativeSocket(Socket socket) {
                if (socket instanceof NativeSocketBase nativeSocket)
                    return nativeSocket;
                throw new IllegalArgumentException(
                    "socket is not backed by the native zlink runtime");
            }
        });
    }

    protected NativeSocketBase(Context ctx, SocketType type) {
        Objects.requireNonNull(ctx, "ctx");
        Objects.requireNonNull(type, "type");
        this.options = ContractAccess.commonSocketOptions(this);
        this.runtime = new NativeSocketRuntime(ctx, type);
    }

    protected NativeSocketBase(MemorySegment handle, boolean own,
                     SocketType socketTypeHint) {
        Objects.requireNonNull(handle, "handle");
        this.options = ContractAccess.commonSocketOptions(this);
        this.runtime = new NativeSocketRuntime(handle, own, socketTypeHint);
    }

    protected final NativeSocketRuntime runtime() {
        return runtime;
    }

    public CommonSocketOptions options() { return options; }
    public SocketMonitor monitorOpen() { return runtime.monitorOpen(); }
    public SocketMonitor monitorOpen(MonitorEventType... events) { return runtime.monitorOpen(events); }
    public final void setTlsServer(String certPem, String keyPem,
                                   boolean requireClientCert) { runtime.setTlsServer(certPem, keyPem, requireClientCert); }
    public final void setTlsClient(String caCertPem, String hostname,
                                   boolean trustSystem) { runtime.setTlsClient(caCertPem, hostname, trustSystem); }
    public void setSendReadyHandler(SendReadyHandler handler) { runtime.setSendReadyHandler(handler); }
    @Override public void close() { runtime.close(); }

    MemorySegment nativeHandle() { return runtime.handle(); }
    MemorySegment handle() { return runtime.handle(); }
}
