package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

final class ZLinkChannelSubmissionAssertions {
    private ZLinkChannelSubmissionAssertions() {}

    // 01-execution/01-submit-and-completion.ko.md:50: submit reports terminal failure
    // asynchronously.
    static <T extends Throwable> T assertSubmitFailure(
            Class<T> type, Supplier<? extends CompletionStage<?>> submit) {
        CompletionException failure =
                assertThrows(
                        CompletionException.class, () -> submit.get().toCompletableFuture().join());
        return assertInstanceOf(type, failure.getCause());
    }
}
