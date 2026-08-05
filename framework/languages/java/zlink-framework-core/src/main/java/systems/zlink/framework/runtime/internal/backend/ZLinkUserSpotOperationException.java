package systems.zlink.framework.runtime.internal.backend;

/**
 * Internal terminal result carried from the User Spot runtime to the service
 * transport. This type is not part of the public application contract.
 */
public final class ZLinkUserSpotOperationException extends RuntimeException {
    private final int terminalResult;
    private final int failureCode;

    public ZLinkUserSpotOperationException(
        int terminalResult,
        int failureCode,
        String message) {
        super(message);
        if (terminalResult <= 0 || failureCode < 0) {
            throw new IllegalArgumentException(
                "terminalResult must be positive and failureCode non-negative");
        }
        this.terminalResult = terminalResult;
        this.failureCode = failureCode;
    }

    public int terminalResult() {
        return terminalResult;
    }

    public int failureCode() {
        return failureCode;
    }
}
