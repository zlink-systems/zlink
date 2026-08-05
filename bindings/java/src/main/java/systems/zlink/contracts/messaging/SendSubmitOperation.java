/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.sockets.SendFlags;
/** Accepts further parts, flags, and the terminal submit of a send builder. */
public interface SendSubmitOperation
  extends MessageBuilderStage<SendSubmitOperation> {
    /**
     * Adds another message part; consumed on a successful submit (see
     * {@link SendOperation} for the ownership contract).
     *
     * @param part the message part
     * @return this operation for chaining
     */
    SendSubmitOperation message(Message part);

    /**
     * Sets the send flags, replacing any previously set flags.
     *
     * @param flags the flags to apply at submit time
     * @return this operation for chaining
     */
    SendSubmitOperation flags(SendFlags flags);

    /**
     * Submits the accumulated parts.
     *
     * @return {@code true} when the parts were queued; {@code false} only when
     *         {@link SendFlags#DONT_WAIT} is set and the send would have
     *         blocked (back-pressure); other failures throw
     *         {@link systems.zlink.contracts.errors.ZlinkException}
     */
    boolean submit();
}
