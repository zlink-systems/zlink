package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.messaging.Message;

import java.util.List;

final class ZLinkJavaBackendCodec {
    private ZLinkJavaBackendCodec() {}

    static List<Message> copyParts(List<Message> parts) {
        return parts.stream().map(Message::from).toList();
    }
}
