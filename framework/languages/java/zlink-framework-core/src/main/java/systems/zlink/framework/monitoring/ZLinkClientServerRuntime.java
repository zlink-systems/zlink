package systems.zlink.framework.monitoring;

import java.util.concurrent.Flow;

public interface ZLinkClientServerRuntime {
    ZLinkClientServerStatus snapshot(String channelName);

    Flow.Publisher<ZLinkObservedStatus<ZLinkClientServerStatus>> observe(
        String channelName,
        int capacity);

    boolean isReady(String channelName);
}
