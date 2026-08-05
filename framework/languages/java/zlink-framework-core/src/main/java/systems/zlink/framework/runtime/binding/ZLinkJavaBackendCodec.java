package systems.zlink.framework.runtime.binding;

import java.util.List;
import systems.zlink.contracts.messaging.Message;

final class ZLinkJavaBackendCodec {
    private ZLinkJavaBackendCodec() {
    }

    static List<Message> copyParts(List<Message> parts) {
        return parts.stream()
            .map(Message::from)
            .toList();
    }
}
