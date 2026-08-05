/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.messaging.RequestCallbackSubmitOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;

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

    public static RequestOperation request(RequestAsyncInvoker asyncInvoker,
                                           RequestCallbackInvoker callbackInvoker) {
        return new RequestBuilder(asyncInvoker, callbackInvoker);
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
    public interface RequestAsyncInvoker {
        CompletableFuture<List<Message>> submit(List<Message> parts,
                                                SendFlags flags,
                                                Duration timeout);
    }

    @FunctionalInterface
    public interface RequestCallbackInvoker {
        boolean submit(List<Message> parts, RequestCallback callback,
                       SendFlags flags, Duration timeout);
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
        private final RequestCallbackInvoker callbackInvoker;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private Duration timeout = Duration.ofMillis(DEFAULT_TIMEOUT_MS);
        private SendFlags flags = SendFlags.NONE;
        private boolean submitted;

        private RequestBuilder(RequestAsyncInvoker asyncInvoker,
                               RequestCallbackInvoker callbackInvoker) {
            this.asyncInvoker = Objects.requireNonNull(asyncInvoker,
                "asyncInvoker");
            this.callbackInvoker = Objects.requireNonNull(callbackInvoker,
                "callbackInvoker");
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
        public RequestCallbackSubmitOperation flags(SendFlags value) {
            ensureNotSubmitted();
            return new CallbackRequestBuilder(this,
                Objects.requireNonNull(value, "flags"));
        }

        @Override
        public CompletableFuture<List<Message>> submit() {
            markSubmitted();
            return asyncInvoker.submit(parts.asList(), flags, timeout);
        }

        @Override
        public boolean submit(RequestCallback callback) {
            markSubmitted();
            return callbackInvoker.submit(parts.asList(),
                Objects.requireNonNull(callback, "callback"), flags, timeout);
        }

        private void addMessage(Message part) {
            ensureNotSubmitted();
            parts.add(Objects.requireNonNull(part, "part"));
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

    private static final class CallbackRequestBuilder
      implements RequestCallbackSubmitOperation {
        private final RequestBuilder source;
        private SendFlags flags;

        private CallbackRequestBuilder(RequestBuilder source, SendFlags flags) {
            this.source = source;
            this.flags = flags;
        }

        @Override
        public RequestCallbackSubmitOperation message(Message part) {
            source.addMessage(part);
            return this;
        }

        @Override
        public RequestCallbackSubmitOperation timeout(Duration value) {
            source.timeout(value);
            return this;
        }

        @Override
        public RequestCallbackSubmitOperation flags(SendFlags value) {
            source.ensureNotSubmitted();
            flags = Objects.requireNonNull(value, "flags");
            return this;
        }

        @Override
        public boolean submit(RequestCallback callback) {
            source.markSubmitted();
            return source.callbackInvoker.submit(source.parts.asList(),
                Objects.requireNonNull(callback, "callback"), flags,
                source.timeout);
        }
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
}
