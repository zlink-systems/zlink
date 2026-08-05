/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.internal.sockets.SocketOptions;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.internal.ContractAccess;
import java.time.Duration;
import systems.zlink.internal.DurationConversions;
import java.util.Objects;
import java.util.Optional;

/** The typed facade over ROUTER-specific socket options. */
public final class RouterSocketOptions extends CommonSocketOptions {
    private static final int OPT_REQUEST_TIMEOUT_MS = 0x3105;
    private static final int OPT_WEIGHT = 0x3106;

    RouterSocketOptions(Socket socket) {
        super(socket);
    }

    public boolean mandatory() {
        return ContractAccess.socketGetOption(socket, SocketOptions.ROUTER_MANDATORY) != 0;
    }

    public void mandatory(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.ROUTER_MANDATORY, enabled ? 1 : 0);
    }

    public boolean handover() {
        return socket.options().ridDuplicatePolicy()
            == RidDuplicatePolicy.HANDOVER;
    }

    public void handover(boolean enabled) {
        socket.options().ridDuplicatePolicy(enabled
            ? RidDuplicatePolicy.HANDOVER
            : RidDuplicatePolicy.REJECT);
    }

    public boolean probe() {
        return ContractAccess.socketGetOption(socket, SocketOptions.PROBE_ROUTER) != 0;
    }

    public void probe(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.PROBE_ROUTER, enabled ? 1 : 0);
    }

    public Optional<RoutingId> connectRoutingId() {
        byte[] value = ContractAccess.socketGetOption(socket, SocketOptions.CONNECT_ROUTING_ID_BYTES);
        return value.length == 0 ? Optional.empty()
            : Optional.of(RoutingId.from(value));
    }

    public void setConnectRoutingId(RoutingId routingId) {
        ContractAccess.socketSetOption(socket, SocketOptions.CONNECT_ROUTING_ID_BYTES,
            routingId == null ? new byte[0] : routingId.toBytes());
    }

    public Duration requestTimeout() {
        return Duration.ofMillis(getIntOption(OPT_REQUEST_TIMEOUT_MS));
    }

    public void requestTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        setIntOption(OPT_REQUEST_TIMEOUT_MS, DurationConversions.toIntMillis(value, "value"));
    }

    public int peerWeight() {
        return getIntOption(OPT_WEIGHT);
    }

    public void peerWeight(int value) {
        setIntOption(OPT_WEIGHT, value);
    }

    private int getIntOption(int option) {
        return ContractAccess.socketGetRouterIntOption(socket, option);
    }

    private void setIntOption(int option, int value) {
        ContractAccess.socketSetRouterIntOption(socket, option, value);
    }
}
