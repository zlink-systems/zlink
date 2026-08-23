/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/** Builds a synchronous PUB/XPUB publish. */
public interface PublishOperation
  extends MessageBuilderStage<PublishSubmitOperation> {
    /** Adds the first message part. */
    PublishSubmitOperation message(Message part);
}
