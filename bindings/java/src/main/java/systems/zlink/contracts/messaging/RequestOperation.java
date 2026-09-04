/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a request: add the request parts, then submit for asynchronous reply
 * completion.
 * An accepted asynchronous operation consumes the parts. If admission is
 * backpressured, the binding retains its own snapshot until the matching
 * WRITABLE token allows a retry. The caller owns any reply parts delivered on
 * completion.
 */
public interface RequestOperation
  extends MessageBuilderStage<RequestSubmitOperation> {
    /**
     * Adds the first request part.
     *
     * @param part the request part; consumed on a successful submit
     * @return the submit stage
     */
    RequestSubmitOperation message(Message part);
}
