/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.internal.sockets.SocketOptions;

import systems.zlink.internal.ContractAccess;

import java.time.Duration;
import systems.zlink.internal.DurationConversions;
import java.util.Objects;

/** The typed facade over socket options shared by every socket type. */
public class CommonSocketOptions {
    final Socket socket;

    static {
        ContractAccess.register(new ContractAccess.SocketOptionFacadesAccess() {
            @Override
            public CommonSocketOptions common(Socket socket) {
                return new CommonSocketOptions(socket);
            }

            @Override
            public DealerSocketOptions dealer(Socket socket) {
                return new DealerSocketOptions(socket);
            }

            @Override
            public PubSocketOptions pub(Socket socket) {
                return new PubSocketOptions(socket);
            }

            @Override
            public RouterSocketOptions router(Socket socket) {
                return new RouterSocketOptions(socket);
            }

            @Override
            public StreamSocketOptions stream(Socket socket) {
                return new StreamSocketOptions(socket);
            }

            @Override
            public SubSocketOptions sub(Socket socket) {
                return new SubSocketOptions(socket);
            }
        });
    }

    CommonSocketOptions(Socket socket) {
        this.socket = socket;
    }

    long affinity() {
        return ContractAccess.socketGetOption(socket, SocketOptions.AFFINITY);
    }

    void affinity(long value) {
        ContractAccess.socketSetOption(socket, SocketOptions.AFFINITY, value);
    }

    int rate() {
        return ContractAccess.socketGetOption(socket, SocketOptions.RATE);
    }

    void rate(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.RATE, value);
    }

    Duration recoveryInterval() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.RECOVERY_IVL));
    }

    void recoveryInterval(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.RECOVERY_IVL, DurationConversions.toIntMillis(value, "value"));
    }

    public Duration linger() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.LINGER));
    }

    public void linger(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.LINGER, DurationConversions.toIntMillis(value, "value"));
    }

    /**
     * Returns the outbound accounted-byte HWM as an unsigned 64-bit value;
     * zero means unlimited. Use {@link Long#toUnsignedString(long)} when a
     * decimal representation above {@link Long#MAX_VALUE} is needed.
     */
    public long sendHwm() {
        return ContractAccess.socketGetOption(socket, SocketOptions.SNDHWM);
    }

    /**
     * Sets the outbound accounted-byte HWM from the unsigned bit pattern in
     * {@code value}; zero means unlimited.
     */
    public void sendHwm(long value) {
        ContractAccess.socketSetOption(socket, SocketOptions.SNDHWM, value);
    }

    /**
     * Returns the inbound accounted-byte HWM as an unsigned 64-bit value;
     * zero means unlimited. Use {@link Long#toUnsignedString(long)} when a
     * decimal representation above {@link Long#MAX_VALUE} is needed.
     */
    public long recvHwm() {
        return ContractAccess.socketGetOption(socket, SocketOptions.RCVHWM);
    }

    /**
     * Sets the inbound accounted-byte HWM from the unsigned bit pattern in
     * {@code value}; zero means unlimited.
     */
    public void recvHwm(long value) {
        ContractAccess.socketSetOption(socket, SocketOptions.RCVHWM, value);
    }

    public int sendBuffer() {
        return ContractAccess.socketGetOption(socket, SocketOptions.SNDBUF);
    }

    public void sendBuffer(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.SNDBUF, value);
    }

    public int recvBuffer() {
        return ContractAccess.socketGetOption(socket, SocketOptions.RCVBUF);
    }

    public void recvBuffer(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.RCVBUF, value);
    }

    public Duration sendTimeout() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.SNDTIMEO));
    }

    public void sendTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.SNDTIMEO, DurationConversions.toIntMillis(value, "value"));
    }

    public Duration recvTimeout() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.RCVTIMEO));
    }

    public void recvTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.RCVTIMEO, DurationConversions.toIntMillis(value, "value"));
    }

    Duration handshakeInterval() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.HANDSHAKE_IVL));
    }

    void handshakeInterval(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.HANDSHAKE_IVL, DurationConversions.toIntMillis(value, "value"));
    }

    public boolean immediate() {
        return ContractAccess.socketGetOption(socket, SocketOptions.IMMEDIATE) != 0;
    }

    public void immediate(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.IMMEDIATE, enabled ? 1 : 0);
    }

    public RidDuplicatePolicy ridDuplicatePolicy() {
        return RidDuplicatePolicy.fromValue(
          ContractAccess.socketGetOption(socket, SocketOptions.RID_DUPLICATE_POLICY));
    }

    public void ridDuplicatePolicy(RidDuplicatePolicy value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.RID_DUPLICATE_POLICY, value.value());
    }

    int routeValueMaxSize() {
        return ContractAccess.socketGetOption(socket, SocketOptions.ROUTE_VALUE_MAX_SIZE);
    }

    void routeValueMaxSize(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.ROUTE_VALUE_MAX_SIZE, value);
    }

    public Duration connectTimeout() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.CONNECT_TIMEOUT));
    }

    public void connectTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.CONNECT_TIMEOUT, DurationConversions.toIntMillis(value, "value"));
    }

    public boolean ipv6() {
        return ContractAccess.socketGetOption(socket, SocketOptions.IPV6) != 0;
    }

    public void ipv6(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.IPV6, enabled ? 1 : 0);
    }

    int tos() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TOS);
    }

    void tos(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TOS, value);
    }

    int multicastHops() {
        return ContractAccess.socketGetOption(socket, SocketOptions.MULTICAST_HOPS);
    }

    void multicastHops(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.MULTICAST_HOPS, value);
    }

    int multicastMaxTpdu() {
        return ContractAccess.socketGetOption(socket, SocketOptions.MULTICAST_MAXTPDU);
    }

    void multicastMaxTpdu(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.MULTICAST_MAXTPDU, value);
    }

    String bindToDevice() {
        return ContractAccess.socketGetOption(socket, SocketOptions.BINDTODEVICE);
    }

    void bindToDevice(String value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.BINDTODEVICE, value);
    }

    public boolean tcpNoDelay() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_NODELAY) != 0;
    }

    public void tcpNoDelay(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_NODELAY, enabled ? 1 : 0);
    }

    public int tcpKeepalive() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_KEEPALIVE);
    }

    public void tcpKeepalive(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_KEEPALIVE, value);
    }

    int tcpKeepaliveCount() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_KEEPALIVE_CNT);
    }

    void tcpKeepaliveCount(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_KEEPALIVE_CNT, value);
    }

    int tcpKeepaliveIdle() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_KEEPALIVE_IDLE);
    }

    void tcpKeepaliveIdle(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_KEEPALIVE_IDLE, value);
    }

    int tcpKeepaliveInterval() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_KEEPALIVE_INTVL);
    }

    void tcpKeepaliveInterval(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_KEEPALIVE_INTVL, value);
    }

    int tcpMaxRt() {
        return ContractAccess.socketGetOption(socket, SocketOptions.TCP_MAXRT);
    }

    void tcpMaxRt(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.TCP_MAXRT, value);
    }

    boolean conflate() {
        return ContractAccess.socketGetOption(socket, SocketOptions.CONFLATE) != 0;
    }

    void conflate(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.CONFLATE, enabled ? 1 : 0);
    }

    boolean blocky() {
        return ContractAccess.socketGetOption(socket, SocketOptions.BLOCKY) != 0;
    }

    void blocky(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.BLOCKY, enabled ? 1 : 0);
    }

    boolean invertMatching() {
        return ContractAccess.socketGetOption(socket, SocketOptions.INVERT_MATCHING) != 0;
    }

    void invertMatching(boolean enabled) {
        ContractAccess.socketSetOption(socket, SocketOptions.INVERT_MATCHING, enabled ? 1 : 0);
    }

    public long maxMessageSize() {
        return ContractAccess.socketGetOption(socket, SocketOptions.MAXMSGSIZE);
    }

    public void maxMessageSize(long value) {
        ContractAccess.socketSetOption(socket, SocketOptions.MAXMSGSIZE, value);
    }

    public int backlog() {
        return ContractAccess.socketGetOption(socket, SocketOptions.BACKLOG);
    }

    public void backlog(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.BACKLOG, value);
    }

    public Duration reconnectInterval() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.RECONNECT_IVL));
    }

    public void reconnectInterval(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.RECONNECT_IVL, DurationConversions.toIntMillis(value, "value"));
    }

    public Duration reconnectIntervalMax() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.RECONNECT_IVL_MAX));
    }

    public void reconnectIntervalMax(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.RECONNECT_IVL_MAX, DurationConversions.toIntMillis(value, "value"));
    }

    public SubmitRetryMode submitRetryMode() {
        return SubmitRetryMode.fromValue(
            ContractAccess.socketGetOption(socket, SocketOptions.SUBMIT_RETRY_MODE));
    }

    public void submitRetryMode(SubmitRetryMode value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.SUBMIT_RETRY_MODE, value.value());
    }

    public Duration submitRetryTimeout() {
        return Duration.ofMillis(ContractAccess.socketGetOption(socket, SocketOptions.SUBMIT_RETRY_TIMEOUT));
    }

    public void submitRetryTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.SUBMIT_RETRY_TIMEOUT, DurationConversions.toIntMillis(value, "value"));
    }

    public int submitRetryAttempts() {
        return ContractAccess.socketGetOption(socket, SocketOptions.SUBMIT_RETRY_ATTEMPTS);
    }

    public void submitRetryAttempts(int value) {
        ContractAccess.socketSetOption(socket, SocketOptions.SUBMIT_RETRY_ATTEMPTS, value);
    }

    int fd() {
        return ContractAccess.socketGetOption(socket, SocketOptions.FD);
    }

    int events() {
        return ContractAccess.socketGetOption(socket, SocketOptions.EVENTS);
    }

    SocketType socketType() {
        return SocketType.fromValue(ContractAccess.socketGetOption(socket, SocketOptions.TYPE));
    }

    public String lastEndpoint() {
        return ContractAccess.socketGetOption(socket, SocketOptions.LAST_ENDPOINT);
    }

    void zmpMetadata(String value) {
        Objects.requireNonNull(value, "value");
        ContractAccess.socketSetOption(socket, SocketOptions.ZMP_METADATA, value);
    }
}
