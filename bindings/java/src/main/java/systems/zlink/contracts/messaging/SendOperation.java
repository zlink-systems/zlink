/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a multipart send: add one or more parts, then
 * {@link SendSubmitOperation#submit()}.
 *
 * <p>Submitting consumes the added {@link Message} parts. On a successful
 * submit each part is moved into the transport; on failure, ownership of
 * every part is restored to the caller for retry or disposal.
 */
public interface SendOperation
  extends MessageBuilderStage<SendSubmitOperation> {
    /**
     * Adds the first message part.
     *
     * @param part the message part; consumed on a successful submit
     * @return the submit stage
     */
    SendSubmitOperation message(Message part);
}
