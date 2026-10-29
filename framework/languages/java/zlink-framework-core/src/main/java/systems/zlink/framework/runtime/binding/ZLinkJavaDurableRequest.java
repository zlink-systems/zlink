package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerRouteObservation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Owns readiness for one registered lifecycle request. */
public final class ZLinkJavaDurableRequest<T> {
    public static Throwable requestFailure(Throwable failure) {
        return ZLinkJavaRawMeshNode.requestFailure(failure);
    }

    @FunctionalInterface
    public interface Submit<T> {
        CompletionStage<?> apply(
                List<byte[]> frames,
                Duration remaining,
                CompletableFuture<Void> admission,
                BiConsumer<T, Throwable> terminal);
    }

    private static final Logger LOGGER = Logger.getLogger(ZLinkJavaDurableRequest.class.getName());
    private final Supplier<List<byte[]>> prepare;
    private final Submit<T> submit;
    private final Supplier<PeerRouteObservation> route;
    private final Function<Runnable, CompletionStage<Void>> owner;
    private final ZLinkServiceOperationRegistry operations;
    private final UUID id;
    private final BiConsumer<T, Throwable> terminal;
    private final CompletableFuture<Void> admission;
    private CompletionStage<?> pending;
    private AutoCloseable observation;

    private ZLinkJavaDurableRequest(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            Function<Runnable, CompletionStage<Void>> owner,
            ZLinkServiceOperationRegistry operations,
            UUID id,
            BiConsumer<T, Throwable> terminal,
            CompletableFuture<Void> admission) {
        this.admission = admission;
        this.prepare = prepare;
        this.submit = submit;
        this.route = route;
        this.owner = owner;
        this.operations = operations;
        this.id = id;
        this.terminal = terminal;
    }

    public static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations) {
        return request(prepare, submit, route, observe, owner, timeout, operations, null);
    }

    static <T> CompletionStage<T> request(
            Supplier<List<byte[]>> prepare,
            Submit<T> submit,
            Supplier<PeerRouteObservation> route,
            BiFunction<Runnable, Runnable, AutoCloseable> observe,
            Function<Runnable, CompletionStage<Void>> owner,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            CompletableFuture<Void> requestAdmission) {
        CompletableFuture<Void> admission =
                requestAdmission == null ? new CompletableFuture<>() : requestAdmission;
        UUID id = ZLinkServiceOperationIds.next();
        var caller = operations.<T>prepareOperation(id);
        if (requestAdmission != null)
            caller.whenComplete(
                    (reply, failure) -> {
                        if (failure == null) requestAdmission.complete(null);
                        else requestAdmission.completeExceptionally(failure);
                    });
        operations.submitTerminal(
                id,
                timeout,
                terminal -> {
                    var request =
                            new ZLinkJavaDurableRequest<>(
                                    prepare,
                                    submit,
                                    route,
                                    owner,
                                    operations,
                                    id,
                                    terminal,
                                    admission);
                    // Logical completion releases readiness; the binding retains its physical
                    // receiver.
                    caller.whenComplete((reply, failure) -> request.post(request::release));
                    request.post(
                            () -> {
                                if (!operations.isPending(id)) return;
                                request.observation =
                                        observe.apply(
                                                () -> request.post(request::attempt),
                                                request::endTarget);
                                request.attempt();
                            });
                    return null;
                },
                ignored -> {},
                caller);
        return caller;
    }

    private void post(Runnable action) {
        try {
            owner.apply(action)
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) operations.completeExceptionally(id, failure);
                            });
        } catch (RuntimeException failure) {
            operations.completeExceptionally(id, failure);
        }
    }

    private void attempt() {
        if (!operations.isPending(id) || pending != null || !route.get().ready()) return;
        Duration remaining = operations.remainingTimeout(id);
        if (remaining.isZero()) return;
        try {
            List<byte[]> frames = prepare.get();
            if (frames == null) return;
            remaining = operations.remainingTimeout(id);
            if (remaining.isZero() || !operations.isPending(id)) return;
            // §5: after this binding operation starts, route changes cannot resubmit it.
            pending =
                    Objects.requireNonNull(
                            submit.apply(
                                    frames,
                                    remaining,
                                    admission,
                                    (reply, failure) ->
                                            terminal.accept(
                                                    reply,
                                                    failure == null
                                                            ? null
                                                            : terminalFailure(failure))),
                            "binding request");
        } catch (RuntimeException failure) {
            terminal.accept(null, terminalFailure(failure));
        }
    }

    private static Throwable terminalFailure(Throwable failure) {
        Throwable cause = ZLinkJavaRawMeshNode.unwrap(failure);
        // Preserve the existing lifecycle caller's typed initial rejection.
        return cause instanceof ZlinkSubmitException submit
                        && submit.getResult() == SubmitResult.NOT_ADMITTED
                ? cause
                : requestFailure(cause);
    }

    private void endTarget() {
        terminal.accept(
                null,
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "durable request target lifecycle ended"));
    }

    private void release() {
        if (observation != null) {
            try {
                observation.close();
            } catch (Exception failure) {
                LOGGER.log(Level.WARNING, "durable request observer cleanup failed", failure);
            }
            observation = null;
        }
        pending = null;
    }
}
