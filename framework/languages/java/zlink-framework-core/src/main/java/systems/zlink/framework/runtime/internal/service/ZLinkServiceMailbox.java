package systems.zlink.framework.runtime.internal.service;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;

/** Bounded owner-serialized mailbox with an independent infrastructure reserve. */
public final class ZLinkServiceMailbox implements AutoCloseable {
    private final DomainState application;
    private final DomainState infrastructure;
    private long nextClaimSerial = 1;
    private boolean closed;

    public ZLinkServiceMailbox(
        long applicationMessageBudget,
        long applicationByteBudget,
        long infrastructureMessageBudget,
        long infrastructureByteBudget) {
        application = new DomainState(
            applicationMessageBudget, applicationByteBudget);
        infrastructure = new DomainState(
            infrastructureMessageBudget, infrastructureByteBudget);
    }

    public synchronized boolean tryEnqueue(Record record) {
        Objects.requireNonNull(record, "record");
        if (closed) {
            return false;
        }
        DomainState domain = domain(record.domain());
        long bytes = record.retainedBytes();
        if (domain.messages >= domain.messageBudget
            || bytes > domain.byteBudget - domain.bytes) {
            return false;
        }
        OwnerQueue queue =
            domain.owners.computeIfAbsent(record.owner(), ignored -> new OwnerQueue());
        queue.records.addLast(record.copy());
        queue.bytes += bytes;
        domain.messages++;
        domain.bytes += bytes;
        if (!queue.claimed && domain.indexed.add(record.owner())) {
            domain.ready.addLast(record.owner());
        }
        return true;
    }

    public synchronized Optional<Claim> tryClaim(
        Domain domainValue,
        int messageBudget,
        long byteBudget) {
        if (messageBudget <= 0 || byteBudget <= 0) {
            throw new IllegalArgumentException("claim budgets must be positive");
        }
        DomainState domain = domain(domainValue);
        while (!domain.ready.isEmpty()) {
            String owner = domain.ready.removeFirst();
            domain.indexed.remove(owner);
            OwnerQueue queue = domain.owners.get(owner);
            if (queue == null || queue.claimed || queue.records.isEmpty()) {
                continue;
            }
            queue.claimed = true;
            queue.claimSerial = allocateClaimSerial();
            List<Record> records = new ArrayList<>();
            long bytes = 0;
            while (!queue.records.isEmpty() && records.size() < messageBudget) {
                Record next = queue.records.peekFirst();
                long nextBytes = next.retainedBytes();
                if (!records.isEmpty() && nextBytes > byteBudget - bytes) {
                    break;
                }
                queue.records.removeFirst();
                queue.bytes -= nextBytes;
                domain.messages--;
                domain.bytes -= nextBytes;
                bytes += nextBytes;
                records.add(next);
            }
            return Optional.of(new Claim(
                owner,
                domainValue,
                queue.claimSerial,
                List.copyOf(records)));
        }
        return Optional.empty();
    }

    public synchronized boolean release(Claim claim) {
        Objects.requireNonNull(claim, "claim");
        DomainState domain = domain(claim.domain());
        OwnerQueue queue = domain.owners.get(claim.owner());
        if (queue == null
            || !queue.claimed
            || queue.claimSerial != claim.serial()) {
            return false;
        }
        queue.claimed = false;
        queue.claimSerial = 0;
        if (queue.records.isEmpty()) {
            domain.owners.remove(claim.owner());
        } else if (domain.indexed.add(claim.owner())) {
            domain.ready.addLast(claim.owner());
        }
        return true;
    }

    public synchronized long pendingMessages(Domain value) {
        return domain(value).messages;
    }

    public synchronized long pendingBytes(Domain value) {
        return domain(value).bytes;
    }

    @Override
    public synchronized void close() {
        if (closed) {
            return;
        }
        closed = true;
        application.clear();
        infrastructure.clear();
    }

    private long allocateClaimSerial() {
        if (nextClaimSerial <= 0) {
            throw new IllegalStateException("mailbox claim serial is exhausted");
        }
        return nextClaimSerial++;
    }

    private DomainState domain(Domain value) {
        return switch (Objects.requireNonNull(value, "domain")) {
            case APPLICATION -> application;
            case INFRASTRUCTURE -> infrastructure;
        };
    }

    public enum Domain {
        APPLICATION,
        INFRASTRUCTURE
    }

    public record Record(
        String owner,
        Domain domain,
        List<byte[]> parts,
        byte[] sourceRoutingId,
        Long requestSequence,
        Long correlation) {
        public Record {
            if (owner == null || owner.isBlank()) {
                throw new IllegalArgumentException("owner is required");
            }
            Objects.requireNonNull(domain, "domain");
            parts = copyParts(parts);
            sourceRoutingId =
                sourceRoutingId == null ? null : sourceRoutingId.clone();
        }

        @Override
        public List<byte[]> parts() {
            return copyParts(parts);
        }

        @Override
        public byte[] sourceRoutingId() {
            return sourceRoutingId == null ? null : sourceRoutingId.clone();
        }

        long retainedBytes() {
            return parts.stream().mapToLong(part -> part.length).sum();
        }

        Record copy() {
            return new Record(
                owner,
                domain,
                parts,
                sourceRoutingId,
                requestSequence,
                correlation);
        }

        private static List<byte[]> copyParts(List<byte[]> values) {
            Objects.requireNonNull(values, "parts");
            return values.stream()
                .map(value -> Objects.requireNonNull(value, "part").clone())
                .toList();
        }
    }

    public record Claim(
        String owner,
        Domain domain,
        long serial,
        List<Record> records) {
    }

    private static final class DomainState {
        private final Map<String, OwnerQueue> owners = new HashMap<>();
        private final Deque<String> ready = new ArrayDeque<>();
        private final Set<String> indexed = new HashSet<>();
        private final long messageBudget;
        private final long byteBudget;
        private long messages;
        private long bytes;

        private DomainState(long messageBudget, long byteBudget) {
            if (messageBudget <= 0 || byteBudget <= 0) {
                throw new IllegalArgumentException(
                    "mailbox budgets must be positive");
            }
            this.messageBudget = messageBudget;
            this.byteBudget = byteBudget;
        }

        private void clear() {
            owners.clear();
            ready.clear();
            indexed.clear();
            messages = 0;
            bytes = 0;
        }
    }

    private static final class OwnerQueue {
        private final Deque<Record> records = new ArrayDeque<>();
        private long bytes;
        private boolean claimed;
        private long claimSerial;
    }
}
