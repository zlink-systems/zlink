package systems.zlink.e2e.registrymessaging.shared;

import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import systems.zlink.framework.errors.ZLinkFrameworkException;

public final class FailureEvidence {
    private FailureEvidence() {
    }

    public static Contracts.RequestFailureRes from(Throwable error) {
        if (error == null) {
            return new Contracts.RequestFailureRes(false, "");
        }
        Throwable current = error;
        while ((current instanceof CompletionException || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        String kind = current instanceof ZLinkFrameworkException frameworkError
            ? frameworkError.kind().name()
            : current.getClass().getSimpleName();
        return new Contracts.RequestFailureRes(true, kind);
    }
}
