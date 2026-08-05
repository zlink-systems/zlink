/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.internal.ContractAccess;
import java.time.Duration;
import systems.zlink.internal.DurationConversions;
import java.util.Objects;

/** The typed facade over DEALER-specific socket options. */
public final class DealerSocketOptions extends CommonSocketOptions {
    private static final int OPT_PROBE = 0x3201;
    private static final int OPT_REQUEST_TIMEOUT_MS = 0x3202;
    private static final int OPT_WEIGHT = 0x3203;

    DealerSocketOptions(Socket socket) {
        super(socket);
    }

    public boolean probe() {
        return ContractAccess.socketGetDealerIntOption(socket, OPT_PROBE) != 0;
    }

    public void probe(boolean enabled) {
        setIntOption(OPT_PROBE, enabled ? 1 : 0);
    }

    public void requestTimeout(Duration value) {
        Objects.requireNonNull(value, "value");
        setIntOption(OPT_REQUEST_TIMEOUT_MS, DurationConversions.toIntMillis(value, "value"));
    }

    public void peerWeight(int value) {
        setIntOption(OPT_WEIGHT, value);
    }

    private void setIntOption(int option, int value) {
        ContractAccess.socketSetDealerIntOption(socket, option, value);
    }
}
