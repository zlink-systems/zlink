/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.messaging.AsyncSendOperation;
import systems.zlink.contracts.messaging.AsyncSendSubmitOperation;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.PublishSubmitOperation;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.RoutedSendOperation;
import systems.zlink.contracts.messaging.RoutedSendSubmitOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;

public final class MessageOperations {
    private static final long DEFAULT_TIMEOUT_MS = 5_000L;

    private MessageOperations() {
    }

    public static SendOperation send(SendInvoker invoker) {
        return new SendBuilder(null, invoker);
    }

    public static SendOperation send(SingleSendInvoker singleInvoker,
                                     SendInvoker invoker) {
        return new SendBuilder(
            Objects.requireNonNull(singleInvoker, "singleInvoker"),
            Objects.requireNonNull(invoker, "invoker"));
    }

    public static RoutedSendOperation routedSend(RoutedSendInvoker invoker,
                                                  SyncSendInvoker syncInvoker) {
        return new RoutedSendBuilder(invoker, syncInvoker);
    }

    public static AsyncSendOperation asyncSend(AsyncSendInvoker invoker,
                                               SyncSendInvoker syncInvoker) {
        return new AsyncSendBuilder(invoker, syncInvoker);
    }

    public static PublishOperation publish(PublishInvoker invoker) {
        return new PublishBuilder(invoker);
    }

    public static RequestOperation request(RequestAsyncInvoker asyncInvoker,
                                           RequestSyncInvoker syncInvoker) {
        return new RequestBuilder(asyncInvoker, syncInvoker);
    }

    public static ReplyOperation reply(ReplyInvoker invoker) {
        return new ReplyBuilder(invoker);
    }

    @FunctionalInterface
    public interface SendInvoker {
        boolean submit(List<Message> parts, SendFlags flags);
    }

    @FunctionalInterface
    public interface SingleSendInvoker {
        boolean submit(Message part, SendFlags flags);
    }

    @FunctionalInterface
    public interface RoutedSendInvoker {
        CompletionStage<Void> submit(List<Message> parts, Duration timeout);
    }

    @FunctionalInterface
    public interface AsyncSendInvoker {
        CompletionStage<Void> submit(List<Message> parts, Duration timeout);
    }

    @FunctionalInterface
    public interface SyncSendInvoker {
        boolean submit(List<Message> parts, SendFlags flags);
    }

    @FunctionalInterface
    public interface PublishInvoker {
        void submit(List<Message> parts, SendFlags flags);
    }

    private static final class AsyncSendBuilder
      implements AsyncSendOperation, AsyncSendSubmitOperation {
        private final AsyncSendInvoker invoker;
        private final SyncSendInvoker syncInvoker;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private Duration timeout;
        private boolean submitted;

        private AsyncSendBuilder(AsyncSendInvoker invoker,
                                 SyncSendInvoker syncInvoker) {
            this.invoker = Objects.requireNonNull(invoker, "invoker");
            this.syncInvoker = Objects.requireNonNull(syncInvoker,
                "syncInvoker");
        }

        @Override
        public AsyncSendSubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public AsyncSendSubmitOperation timeout(Duration value) {
            ensureNotSubmitted();
            timeout = validateTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            return invoker.submit(parts.asList(), timeout);
        }

        @Override
        public void submit_sync(SendFlags flags) {
            markSubmitted();
            if (!syncInvoker.submit(parts.asList(), Objects.requireNonNull(
                flags, "flags"))) {
                throw new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            }
        }

        private void markSubmitted() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    @FunctionalInterface
    public interface RequestAsyncInvoker {
        CompletionStage<List<Message>> submit(List<Message> parts,
                                              Duration timeout);
    }

    @FunctionalInterface
    public interface RequestSyncInvoker {
        CompletionStage<List<Message>> submit(List<Message> parts,
                                              Duration timeout,
                                              SendFlags flags);
    }

    private static final class RoutedSendBuilder
      implements RoutedSendOperation, RoutedSendSubmitOperation {
        private final RoutedSendInvoker invoker;
        private final SyncSendInvoker syncInvoker;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private Duration timeout;
        private boolean submitted;

        private RoutedSendBuilder(RoutedSendInvoker invoker,
                                  SyncSendInvoker syncInvoker) {
            this.invoker = Objects.requireNonNull(invoker, "invoker");
            this.syncInvoker = Objects.requireNonNull(syncInvoker,
                "syncInvoker");
        }

        @Override
        public RoutedSendSubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public RoutedSendSubmitOperation timeout(Duration value) {
            ensureNotSubmitted();
            timeout = validateTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            return invoker.submit(parts.asList(), timeout);
        }

        @Override
        public void submit_sync(SendFlags flags) {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            if (!syncInvoker.submit(parts.asList(), Objects.requireNonNull(
                flags, "flags"))) {
                throw new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
            }
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static final class PublishBuilder
      implements PublishOperation, PublishSubmitOperation {
        private final PublishInvoker invoker;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private SendFlags flags = SendFlags.NONE;
        private boolean submitted;

        private PublishBuilder(PublishInvoker invoker) {
            this.invoker = Objects.requireNonNull(invoker, "invoker");
        }

        @Override
        public PublishSubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public PublishSubmitOperation flags(SendFlags value) {
            ensureNotSubmitted();
            flags = Objects.requireNonNull(value, "flags");
            return this;
        }

        @Override
        public void submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            invoker.submit(parts.asList(), flags);
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    @FunctionalInterface
    public interface ReplyInvoker {
        void submit(List<Message> parts);
    }

    private static final class SendBuilder
      implements SendOperation, SendSubmitOperation {
        private final SingleSendInvoker singleInvoker;
        private final SendInvoker invoker;
        private Message singlePart;
        private MessagePartsBuffer parts;
        private int partCount;
        private SendFlags flags = SendFlags.NONE;
        private boolean submitted;

        private SendBuilder(SingleSendInvoker singleInvoker,
                            SendInvoker invoker) {
            this.singleInvoker = singleInvoker;
            this.invoker = Objects.requireNonNull(invoker, "invoker");
        }

        @Override
        public SendSubmitOperation message(Message part) {
            ensureNotSubmitted();
            Objects.requireNonNull(part, "part");
            if (partCount == 0) {
                singlePart = part;
            } else {
                if (parts == null) {
                    parts = new MessagePartsBuffer();
                    parts.add(singlePart);
                    singlePart = null;
                }
                parts.add(part);
            }
            partCount++;
            return this;
        }

        @Override
        public SendSubmitOperation flags(SendFlags value) {
            ensureNotSubmitted();
            flags = Objects.requireNonNull(value, "flags");
            return this;
        }

        @Override
        public boolean submit() {
            markSubmitted();
            if (partCount == 1) {
                if (singleInvoker != null)
                    return singleInvoker.submit(singlePart, flags);
                return invoker.submit(List.of(singlePart), flags);
            }
            return invoker.submit(parts.asList(), flags);
        }

        private void markSubmitted() {
            ensureNotSubmitted();
            if (partCount == 0)
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static final class RequestBuilder
      implements RequestOperation, RequestSubmitOperation {
        private final RequestAsyncInvoker asyncInvoker;
        private final RequestSyncInvoker syncInvoker;
        private Message singlePart;
        private MessagePartsBuffer parts;
        private int partCount;
        private Duration timeout = Duration.ofMillis(DEFAULT_TIMEOUT_MS);
        private boolean submitted;

        private RequestBuilder(RequestAsyncInvoker asyncInvoker,
                               RequestSyncInvoker syncInvoker) {
            this.asyncInvoker = Objects.requireNonNull(asyncInvoker,
                "asyncInvoker");
            this.syncInvoker = Objects.requireNonNull(syncInvoker,
                "syncInvoker");
        }

        @Override
        public RequestSubmitOperation message(Message part) {
            addMessage(part);
            return this;
        }

        @Override
        public RequestSubmitOperation timeout(Duration value) {
            ensureNotSubmitted();
            timeout = Objects.requireNonNull(value, "timeout");
            return this;
        }

        @Override
        public CompletionStage<List<Message>> submit() {
            markSubmitted();
            return asyncInvoker.submit(requestParts(), timeout);
        }

        @Override
        public List<Message> submit_sync(SendFlags flags) {
            markSubmitted();
            try {
                return syncInvoker.submit(requestParts(), timeout,
                    Objects.requireNonNull(flags, "flags"))
                    .toCompletableFuture().join();
            } catch (CompletionException failure) {
                throw rethrowCompletion(failure);
            }
        }

        @Override
        public void submit_sync(
                SendFlags flags,
                BiConsumer<RequestResult, List<Message>> callback) {
            markSubmitted();
            Objects.requireNonNull(callback, "callback");
            syncInvoker.submit(requestParts(), timeout,
                Objects.requireNonNull(flags, "flags"))
                .whenComplete((reply, failure) -> {
                    if (failure == null) {
                        callback.accept(RequestResult.OK, reply);
                        return;
                    }
                    Throwable cause = completionCause(failure);
                    if (cause instanceof ZlinkRequestException request) {
                        callback.accept(request.getResult(), List.of());
                        return;
                    }
                    callback.accept(RequestResult.INTERNAL_ERROR, List.of());
                });
        }

        private void addMessage(Message part) {
            ensureNotSubmitted();
            Objects.requireNonNull(part, "part");
            if (partCount == 0) {
                singlePart = part;
            } else {
                if (parts == null) {
                    parts = new MessagePartsBuffer();
                    parts.add(singlePart);
                    singlePart = null;
                }
                parts.add(part);
            }
            partCount++;
        }

        private void markSubmitted() {
            ensureNotSubmitted();
            if (partCount == 0)
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
        }

        private List<Message> requestParts() {
            return partCount == 1 ? List.of(singlePart) : parts.asList();
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static RuntimeException rethrowCompletion(
            CompletionException failure) {
        Throwable cause = completionCause(failure);
        if (cause instanceof RuntimeException runtime) {
            return runtime;
        }
        return failure;
    }

    private static Throwable completionCause(Throwable failure) {
        Throwable cause = failure;
        while (cause instanceof CompletionException
               && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
    }

    private static final class ReplyBuilder
      implements ReplyOperation, ReplySubmitOperation {
        private final ReplyInvoker invoker;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private boolean submitted;

        private ReplyBuilder(ReplyInvoker invoker) {
            this.invoker = Objects.requireNonNull(invoker, "invoker");
        }

        @Override
        public ReplySubmitOperation message(Message part) {
            ensureNotSubmitted();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public void submit() {
            ensureNotSubmitted();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            invoker.submit(parts.asList());
        }

        private void ensureNotSubmitted() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static Duration validateTimeout(Duration value) {
        if (value != null && value.isNegative())
            throw new IllegalArgumentException("timeout must not be negative");
        return value;
    }
}
