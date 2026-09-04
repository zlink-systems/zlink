/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a multipart send: add one or more parts, then
 * {@link SendSubmitOperation#submit()}.
 *
 * <p>An accepted submit consumes the added {@link Message} parts. For an
 * asynchronous submit the binding keeps its own packet snapshot while the
 * returned stage is incomplete, so callers may close their consumed wrappers
 * immediately. A failure thrown before the operation is accepted leaves the
 * parts caller-owned; a later exceptional stage does not restore them.
 */
public interface SendOperation
  extends MessageBuilderStage<SendSubmitOperation> {
    /**
     * Adds the first message part.
     *
     * @param part the message part; consumed when submit accepts the operation
     * @return the submit stage
     */
    SendSubmitOperation message(Message part);
}
