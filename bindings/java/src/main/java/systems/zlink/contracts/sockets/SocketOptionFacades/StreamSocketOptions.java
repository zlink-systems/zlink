/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.internal.sockets.SocketOptions;

import systems.zlink.internal.ContractAccess;


/** The typed facade over STREAM-specific socket options. */
public final class StreamSocketOptions extends CommonSocketOptions {
    StreamSocketOptions(Socket socket) {
        super(socket);
    }

    public boolean notifyEnabled() {
        return ContractAccess.socketGetOption(socket, SocketOptions.STREAM_NOTIFY) != 0;
    }

    public void notify(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.STREAM_NOTIFY, enabled ? 1 : 0);
    }

    public StreamRecvMode recvMode() {
        return StreamRecvMode.fromValue(ContractAccess.socketGetOption(socket,
            SocketOptions.STREAM_RECV_MODE));
    }

    public void recvMode(StreamRecvMode mode) {
        ContractAccess.socketSetOption(socket, SocketOptions.STREAM_RECV_MODE,
            java.util.Objects.requireNonNull(mode, "mode").value());
    }
}
