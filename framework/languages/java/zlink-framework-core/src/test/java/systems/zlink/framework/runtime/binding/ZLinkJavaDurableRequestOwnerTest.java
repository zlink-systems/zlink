package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaDurableRequestOwnerTest {
    @Test
    void terminalPublishesRegisteredContinuationOutsideTheNodeOwner() throws Exception {
        var owner = new ZLinkStateLane();
        var reply = List.of(new byte[] {1});
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
                        .thenAccept(
                                value -> {
                                    assertFalse(owner.isOnLane());
                                    assertSame(reply, value);
                                });
        owner.awaitTurn(() -> pending.complete(reply));
        checked.toCompletableFuture().get(2, TimeUnit.SECONDS);
    }
}
