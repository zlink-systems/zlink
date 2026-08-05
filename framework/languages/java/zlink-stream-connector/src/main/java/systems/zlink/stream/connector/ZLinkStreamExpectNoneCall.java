package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkStreamExpectNoneCall {
    ZLinkStreamExpectNoneCall within(Duration window);

    CompletionStage<Void> submit();
}
