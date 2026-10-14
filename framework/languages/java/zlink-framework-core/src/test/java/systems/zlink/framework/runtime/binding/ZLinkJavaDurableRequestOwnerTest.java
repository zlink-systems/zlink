package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaDurableRequestOwnerTest {
    @RepeatedTest(100)
    void terminalPublishesRegisteredContinuationOutsideTheNodeOwner() throws Exception {
        assertPublicationOutsideOwner(false);
    }

    @RepeatedTest(100)
    void terminalFailurePublishesRegisteredContinuationOutsideTheNodeOwner() throws Exception {
        assertPublicationOutsideOwner(true);
    }

    private static void assertPublicationOutsideOwner(boolean failed) throws Exception {
        var owner = new ZLinkStateLane();
        var reply = List.of(new byte[] {1});
        var failure = new IllegalStateException("terminal failed");
        var pending = new CompletableFuture<List<byte[]>>();
        var checked =
                ZLinkJavaDurableRequest.request(
                                () -> reply,
                                (frames, timeout) -> pending,
                                () -> false,
                                listener -> {
                                    listener.run();
                                    return () -> {};
                                },
                                command -> {
                                    if (owner.isOnLane()) {
                                        command.run();
                                        return CompletableFuture.completedFuture(null);
                                    }
                                    return owner.runNowOrQueue(
                                            () -> {
                                                command.run();
                                                return null;
                                            });
                                },
                                Duration.ofSeconds(2))
                        .handle(
                                (value, error) -> {
                                    assertFalse(owner.isOnLane());
                                    if (failed) assertSame(failure, error);
                                    else assertSame(reply, value);
                                    return null;
                                });
        owner.runNowOrQueue(
                        () ->
                                failed
                                        ? pending.completeExceptionally(failure)
                                        : pending.complete(reply))
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
        checked.toCompletableFuture().get(2, TimeUnit.SECONDS);
    }
}
