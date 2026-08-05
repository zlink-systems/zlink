package systems.zlink.stream.connector;

import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

final class DefaultZLinkStreamLifecycleCall implements ZLinkStreamLifecycleCall {
    private final Supplier<CompletionStage<Void>> submitter;

    DefaultZLinkStreamLifecycleCall(Supplier<CompletionStage<Void>> submitter) {
        this.submitter = Objects.requireNonNull(submitter, "submitter");
    }

    @Override
    public CompletionStage<Void> submit() {
        return submitter.get();
    }
}
