/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a request: add the request parts, then submit for asynchronous reply
 * completion.
 * Parts are consumed on a successful submit (see {@link SendOperation} for the
 * ownership contract). The caller owns any reply parts delivered on completion.
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
