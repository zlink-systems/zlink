package systems.zlink.framework.channels;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.Optional;

public interface ZLinkPublishMessageContext extends ZLinkMessageContext {
    String topic();

    Optional<String> source();
}
