package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.function.Predicate;

public interface ZLinkStreamSequenceCall {
    ZLinkStreamSequenceCall expect(
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate);

    <TPayload> ZLinkStreamSequenceCall expect(
        Class<TPayload> payloadType,
        Predicate<ZLinkStreamMessage<TPayload>> predicate);

    ZLinkStreamSequenceCall timeout(Duration timeout);

    CompletionStage<List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>> submit();

    <TPayload> CompletionStage<List<ZLinkStreamMessage<TPayload>>> submit(
        Class<TPayload> payloadType);
}
