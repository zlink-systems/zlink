/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a reply to a received request: add the reply parts, then submit.
 * Parts are consumed on a successful submit (see {@link SendOperation} for the
 * ownership contract).
 */
public interface ReplyOperation
  extends MessageBuilderStage<ReplySubmitOperation> {
    /**
     * Adds the first reply part.
     *
     * @param part the reply part; consumed on a successful submit
     * @return the submit stage
     */
    ReplySubmitOperation message(Message part);
}
