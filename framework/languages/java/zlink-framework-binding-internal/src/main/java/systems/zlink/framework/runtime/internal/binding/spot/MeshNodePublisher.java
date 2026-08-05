/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import java.util.List;

/** A node-level publisher for topic messages. */
public interface MeshNodePublisher extends AutoCloseable {
    /** Publishes a topic message on a channel. */
    void publish(String channel, String topic, List<Message> parts, SendFlags flags);

    /** Publishes with an encoded application-metadata snapshot. */
    void publish(String channel, String topic, byte[] metadata,
                 List<Message> parts, SendFlags flags);

    @Override
    void close();
}
