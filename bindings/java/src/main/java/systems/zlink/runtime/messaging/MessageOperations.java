/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import java.time.Duration;
import java.util.AbstractList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.PublishSubmitOperation;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;

/** Runtime builders that capture their socket target at operation creation. */
public final class MessageOperations {
    private static final Duration DEFAULT_REQUEST_TIMEOUT = Duration.ofSeconds(5);

    private MessageOperations() {
    }

    public static SendOperation send(SendAwaitableInvoker awaitable,
                                     SendBlockingInvoker blocking) {
        return new SendBuilder(awaitable, blocking);
    }

    public static PublishOperation publish(PublishInvoker invoker) {
        return new PublishBuilder(invoker);
    }

    public static RequestOperation request(RequestAwaitableInvoker awaitable,
                                           RequestBlockingInvoker blocking) {
        return new RequestBuilder(awaitable, blocking);
    }

    public static ReplyOperation reply(ReplyInvoker invoker) {
        return new ReplyBuilder(invoker);
    }

    @FunctionalInterface
    public interface SendAwaitableInvoker {
        CompletionStage<Void> submit(List<Message> parts);
    }

    @FunctionalInterface
    public interface SendBlockingInvoker {
        void submit(List<Message> parts);
    }

    @FunctionalInterface
    public interface RequestAwaitableInvoker {
        CompletionStage<List<Message>> submit(List<Message> parts,
                                              Duration timeout);
    }

    @FunctionalInterface
    public interface RequestBlockingInvoker {
        List<Message> submit(List<Message> parts, Duration timeout);
    }

    @FunctionalInterface
    public interface PublishInvoker {
        void submit(List<Message> parts, SendFlags flags);
    }

    @FunctionalInterface
    public interface ReplyInvoker {
        void submit(List<Message> parts);
    }

    private static final class SendBuilder
      implements SendOperation, SendSubmitOperation {
        private final SendAwaitableInvoker awaitable;
        private final SendBlockingInvoker blocking;
        private final MessagePartsBuffer parts = new MessagePartsBuffer();
        private boolean submitted;

        private SendBuilder(SendAwaitableInvoker awaitable,
                            SendBlockingInvoker blocking) {
            this.awaitable = Objects.requireNonNull(awaitable, "awaitable");
            this.blocking = Objects.requireNonNull(blocking, "blocking");
        }

        @Override
        public SendSubmitOperation message(Message part) {
            ensureBuilding();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            finishBuilding();
            return awaitable.submit(parts.asList());
        }

        @Override
        public void submit_sync() {
            finishBuilding();
            blocking.submit(parts.asList());
        }

        private void finishBuilding() {
            ensureBuilding();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
        }

        private void ensureBuilding() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }

    private static final class RequestBuilder extends AbstractList<Message>
      implements RequestOperation, RequestSubmitOperation {
        private final RequestAwaitableInvoker awaitable;
        private final RequestBlockingInvoker blocking;
        private Message first;
        private Message second;
        private MessagePartsBuffer overflow;
        private int count;
        private Duration timeout = DEFAULT_REQUEST_TIMEOUT;
        private boolean submitted;

        private RequestBuilder(RequestAwaitableInvoker awaitable,
                               RequestBlockingInvoker blocking) {
            this.awaitable = Objects.requireNonNull(awaitable, "awaitable");
            this.blocking = Objects.requireNonNull(blocking, "blocking");
        }

        @Override
        public RequestSubmitOperation message(Message part) {
            ensureBuilding();
            Objects.requireNonNull(part, "part");
            if (count == 0) {
                first = part;
            } else if (count == 1) {
                second = part;
            } else {
                if (overflow == null) {
                    overflow = new MessagePartsBuffer();
                    overflow.add(first);
                    overflow.add(second);
                }
                overflow.add(part);
            }
            count++;
            return this;
        }

        @Override
        public RequestSubmitOperation timeout(Duration value) {
            ensureBuilding();
            timeout = Objects.requireNonNull(value, "timeout");
            if (timeout.isNegative())
                throw new IllegalArgumentException("timeout must not be negative");
            return this;
        }

        @Override
        public CompletionStage<List<Message>> submit() {
            finishBuilding();
            return awaitable.submit(requestParts(), timeout);
        }

        @Override
        public List<Message> submit_sync() {
            finishBuilding();
            try {
                return blocking.submit(requestParts(), timeout);
            } catch (CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtime)
                    throw runtime;
                throw failure;
            }
        }

        private List<Message> requestParts() {
            return overflow == null ? this : overflow.asList();
        }

        @Override
        public Message get(int index) {
            if (overflow != null)
                return overflow.get(index);
            if (index == 0 && count > 0)
                return first;
            if (index == 1 && count > 1)
                return second;
            throw new IndexOutOfBoundsException(index);
        }

        @Override
        public int size() {
            return count;
        }

        private void finishBuilding() {
            ensureBuilding();
            if (count == 0)
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
        }

        private void ensureBuilding() {
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
            ensureBuilding();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public PublishSubmitOperation flags(SendFlags value) {
            ensureBuilding();
            flags = Objects.requireNonNull(value, "flags");
            return this;
        }

        @Override
        public void submit() {
            ensureBuilding();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            invoker.submit(parts.asList(), flags);
        }

        private void ensureBuilding() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
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
            ensureBuilding();
            parts.add(Objects.requireNonNull(part, "part"));
            return this;
        }

        @Override
        public void submit() {
            ensureBuilding();
            if (parts.isEmpty())
                throw new IllegalArgumentException("at least one message required");
            submitted = true;
            invoker.submit(parts.asList());
        }

        private void ensureBuilding() {
            if (submitted)
                throw new IllegalStateException("operation already submitted");
        }
    }
}
