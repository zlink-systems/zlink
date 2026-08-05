/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** The outcome of a non-blocking send attempt. */
public enum SendResult {
    SENT,
    BACKPRESSURED,
    NOT_READY
}
