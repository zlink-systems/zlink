/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/** Accepts further parts and the terminal submit of a reply builder. */
public interface ReplySubmitOperation
  extends MessageBuilderStage<ReplySubmitOperation> {
    /**
     * Adds another reply part; consumed on a successful submit (see
     * {@link SendOperation} for the ownership contract).
     *
     * @param part the reply part
     * @return this operation for chaining
     */
    ReplySubmitOperation message(Message part);

    /**
     * Submits the reply. Failures throw
     * {@link systems.zlink.contracts.errors.ZlinkException}.
     */
    void submit();
}
