/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.messaging.Message;
import java.util.List;

/** A reusable buffer that receives message records drained from a claim. */
public interface ReceiveBatch extends AutoCloseable {
    /** Returns the number of message records currently held. */
    int count();

    /** Returns the record metadata at the given index. */
    ReceiveRecord at(int index);

    /** Retains and returns the message parts for the record at the given index. */
    List<Message> retainMessage(int index);

    /** Clears the batch for reuse. */
    void reset();

    @Override
    void close();

    /** Creates a receive batch with the given capacities. */
    static ReceiveBatch create(int messageCapacity, int partCapacity, int byteCapacity) {
        return new FrameworkReceiveBatch(
            messageCapacity, partCapacity, byteCapacity);
    }
}
