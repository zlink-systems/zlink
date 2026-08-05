/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.internal.sockets;


/** The wire type of a socket option value. */
public enum SocketOptionValueType {
    INT32,
    INT64,
    UINT64,
    STRING,
    BYTES
}
