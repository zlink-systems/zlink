package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.function.Predicate;

public interface ZLinkStreamWaitCall {
    ZLinkStreamWaitCall timeout(Duration timeout);

    ZLinkStreamWaitCall where(Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate);

    <TPayload> ZLinkStreamWaitCall where(
        Class<TPayload> payloadType,
        Predicate<ZLinkStreamMessage<TPayload>> predicate);

    CompletionStage<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> submit();

    <TPayload> CompletionStage<ZLinkStreamMessage<TPayload>> submit(Class<TPayload> payloadType);

}
