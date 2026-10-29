/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Core connection snapshot values. */
public enum ConnectionFailureStage {
    NONE,
    RESOLVE,
    TRANSPORT,
    TLS,
    WEBSOCKET,
    CORE_HANDSHAKE,
    ESTABLISHED
}
