package systems.zlink.framework.channels;

import java.util.Optional;
import systems.zlink.framework.ZLinkMessageContext;

public interface ZLinkPublishMessageContext extends ZLinkMessageContext {
    String topic();

    Optional<String> source();
}
