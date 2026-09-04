/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;


/** The outcome of a non-blocking send attempt. */
public enum SendResult {
    /** The packet was admitted. */
    SENT,
    /**
     * A non-blocking attempt was not admitted and reported EAGAIN. Core did
     * not retain the payload, so the caller or binding owns any retry.
     */
    BACKPRESSURED,
    /** The requested target is not ready for a send attempt. */
    NOT_READY
}
