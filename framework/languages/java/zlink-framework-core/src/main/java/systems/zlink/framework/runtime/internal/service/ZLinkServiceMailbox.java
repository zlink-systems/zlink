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

/**
 * Owner-serialized mailbox. Its reservations isolate one owner from another;
 * they are not a second, process-wide transport admission authority.
 */
public final class ZLinkServiceMailbox implements AutoCloseable {
    private static final long RECORD_FIXED_BYTES = 96;
    private static final long PART_FIXED_BYTES = 16;
    private final DomainState application;
    private final DomainState infrastructure;
    private long nextClaimSerial = 1;
    private boolean closed;

    public ZLinkServiceMailbox(
        long applicationOwnerMessageLimit,
        long applicationOwnerByteLimit,
        long infrastructureOwnerMessageLimit,
        long infrastructureOwnerByteLimit) {
        application = new DomainState(
            applicationOwnerMessageLimit,
            applicationOwnerByteLimit);
        infrastructure = new DomainState(
            infrastructureOwnerMessageLimit,
            infrastructureOwnerByteLimit);
    }

    public synchronized boolean tryEnqueue(Record record) {
        Objects.requireNonNull(record, "record");
        if (closed) {
            return false;
        }
        DomainState domain = domain(record.domain());
        long bytes = record.retainedBytes();
        OwnerQueue queue =
            domain.owners.computeIfAbsent(record.owner(), ignored -> new OwnerQueue());
        long reservedBytes;
        long reservedMessages;
        long nextQueueBytes;
        long nextDomainMessages;
        long nextDomainBytes;
        try {
            reservedBytes = Math.addExact(queue.bytes, queue.claimedBytes);
            reservedMessages = Math.addExact(
                queue.records.size(), queue.claimedMessages);
            nextQueueBytes = Math.addExact(queue.bytes, bytes);
            nextDomainMessages = Math.addExact(domain.messages, 1);
            nextDomainBytes = Math.addExact(domain.bytes, bytes);
        } catch (ArithmeticException overflow) {
            removeUnclaimedEmptyOwner(domain, record.owner(), queue);
            return false;
        }
        if (reservedMessages >= domain.ownerMessageLimit
            || reservedBytes > domain.ownerByteLimit
            || bytes > domain.ownerByteLimit - reservedBytes) {
            removeUnclaimedEmptyOwner(domain, record.owner(), queue);
            return false;
        }
        // Record owns immutable copies of all caller-provided byte arrays at
        // construction time. Retain that owned value instead of cloning the
        // full payload a second time on every enqueue.
        queue.records.addLast(record);
        queue.bytes = nextQueueBytes;
        domain.messages = nextDomainMessages;
        domain.bytes = nextDomainBytes;
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
                queue.claimedMessages = Math.addExact(
                    queue.claimedMessages, 1);
                queue.claimedBytes = Math.addExact(
                    queue.claimedBytes, nextBytes);
                bytes = Math.addExact(bytes, nextBytes);
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
        domain.messages -= queue.claimedMessages;
        domain.bytes -= queue.claimedBytes;
        queue.claimedMessages = 0;
        queue.claimedBytes = 0;
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

    private static void removeUnclaimedEmptyOwner(
        DomainState domain,
        String owner,
        OwnerQueue queue) {
        if (queue.records.isEmpty() && !queue.claimed) {
            domain.owners.remove(owner);
        }
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
            long bytes = RECORD_FIXED_BYTES
                + (long) owner.length() * Character.BYTES
                + (sourceRoutingId == null ? 0 : sourceRoutingId.length);
            for (byte[] part : parts) {
                bytes = Math.addExact(bytes, PART_FIXED_BYTES + part.length);
            }
            return bytes;
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
        private final long ownerMessageLimit;
        private final long ownerByteLimit;
        private long messages;
        private long bytes;

        private DomainState(long ownerMessageLimit, long ownerByteLimit) {
            if (ownerMessageLimit <= 0 || ownerByteLimit <= 0) {
                throw new IllegalArgumentException(
                    "owner mailbox limits must be positive");
            }
            this.ownerMessageLimit = ownerMessageLimit;
            this.ownerByteLimit = ownerByteLimit;
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
        private long claimedMessages;
        private long claimedBytes;
        private boolean claimed;
        private long claimSerial;
    }
}
