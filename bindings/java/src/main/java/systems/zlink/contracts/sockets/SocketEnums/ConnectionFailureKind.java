/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

/** Core connection snapshot values. */
public enum ConnectionFailureKind {
    NONE,
    REFUSED,
    ENDPOINT_MISSING,
    TIMED_OUT,
    UNREACHABLE,
    NAME_RESOLUTION,
    AUTH_FAILED,
    PROTOCOL_FAILED,
    CONNECTION_LOST,
    OTHER
}
