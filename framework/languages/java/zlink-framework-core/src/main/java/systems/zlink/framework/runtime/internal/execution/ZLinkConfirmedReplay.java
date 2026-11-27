package systems.zlink.framework.runtime.internal.execution;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.Supplier;

/** A relocation replay keeps its FIFO position until its authority check settles. */
public final class ZLinkConfirmedReplay {
    private ZLinkConfirmedReplay() {}

    public static Supplier<CompletionStage<Void>> after(
            CompletableFuture<Boolean> confirmation, Supplier<CompletionStage<Void>> replay) {
        return confirmation == null ? replay : new Pending(confirmation, replay);
    }

    public static Supplier<CompletionStage<Void>> wrap(
            Supplier<CompletionStage<Void>> replay,
            Function<Supplier<CompletionStage<Void>>, Supplier<CompletionStage<Void>>> wrapper) {
        return replay instanceof Pending pending
                ? new Pending(pending.confirmation(), wrapper.apply(pending.replay()))
                : wrapper.apply(replay);
    }

    public static boolean ready(Supplier<CompletionStage<Void>> replay) {
        return !(replay instanceof Pending pending) || pending.confirmation().isDone();
    }

    public static boolean capturable(Supplier<CompletionStage<Void>> replay) {
        return !(replay instanceof Pending pending) || pending.accepted();
    }

    public static CompletableFuture<Boolean> confirmation(Supplier<CompletionStage<Void>> replay) {
        return replay instanceof Pending pending ? pending.confirmation() : null;
    }

    private record Pending(
            CompletableFuture<Boolean> confirmation, Supplier<CompletionStage<Void>> replay)
            implements Supplier<CompletionStage<Void>> {
        boolean accepted() {
            return confirmation.isDone()
                    && !confirmation.isCompletedExceptionally()
                    && Boolean.TRUE.equals(confirmation.getNow(false));
        }

        @Override
        public CompletionStage<Void> get() {
            return accepted() ? replay.get() : CompletableFuture.completedFuture(null);
        }
    }
}
