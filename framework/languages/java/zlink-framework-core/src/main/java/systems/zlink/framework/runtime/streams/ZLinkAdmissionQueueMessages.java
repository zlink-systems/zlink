package systems.zlink.framework.runtime.streams;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.annotation.JsonDeserialize;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;

import org.jspecify.annotations.Nullable;

import systems.zlink.framework.ZLinkJsonOptional;
import systems.zlink.framework.handlers.ZLinkPacket;

import java.io.IOException;
import java.util.Base64;
import java.util.List;
import java.util.Objects;
import java.util.regex.Pattern;

/**
 * Every Framework-internal Spot message of the login admission queue lives here so the names and
 * shapes change in one place (Authentication Admission §3.5.8). The messages travel between a
 * STREAM node and the queue Instance Spot only and are not client wire. JSON is camelCase, a {@code
 * dec} is a 64-bit integer as a decimal string, a {@code b64} is unpadded base64url, all fields are
 * required unless stated and unknown fields are ignored. A missing field or a wrong form fails
 * decoding, which the receiver treats as a protocol error.
 */
public final class ZLinkAdmissionQueueMessages {
    private ZLinkAdmissionQueueMessages() {}

    /** Instance Spot type of the queue (§3.5.8). */
    public static final String SPOT_TYPE = "$zlink.AdmissionQueue";

    private static final Pattern DEC = Pattern.compile("0|[1-9][0-9]{0,19}");
    private static final Pattern B64 = Pattern.compile("[A-Za-z0-9_-]*");

    /** A {@code dec}: only a JSON string of canonical digits, read as an unsigned 64-bit value. */
    public static final class Dec {
        private Dec() {}

        public static final class Writer extends JsonSerializer<Long> {
            @Override
            public void serialize(Long value, JsonGenerator out, SerializerProvider provider)
                    throws IOException {
                out.writeString(Long.toUnsignedString(value));
            }
        }

        public static final class Reader extends JsonDeserializer<Long> {
            @Override
            public Long deserialize(JsonParser in, DeserializationContext context)
                    throws IOException {
                if (in.currentToken() != JsonToken.VALUE_STRING
                        || !DEC.matcher(in.getText()).matches()) {
                    throw JsonMappingException.from(in, "dec must be a decimal string");
                }
                try {
                    return Long.parseUnsignedLong(in.getText());
                } catch (NumberFormatException failure) {
                    throw JsonMappingException.from(in, "dec exceeds 64 bits");
                }
            }
        }
    }

    /** A {@code b64}: unpadded canonical base64url. */
    public static final class Rid {
        private Rid() {}

        public static final class Reader extends JsonDeserializer<String> {
            @Override
            public String deserialize(JsonParser in, DeserializationContext context)
                    throws IOException {
                if (in.currentToken() != JsonToken.VALUE_STRING) {
                    throw JsonMappingException.from(in, "b64 must be a string");
                }
                return requireB64(in.getText());
            }

            private static String requireB64(String value) throws IOException {
                if (!B64.matcher(value).matches()) {
                    throw new IOException("b64 must be unpadded base64url");
                }
                try {
                    byte[] bytes = Base64.getUrlDecoder().decode(value);
                    if (!Base64.getUrlEncoder()
                            .withoutPadding()
                            .encodeToString(bytes)
                            .equals(value)) {
                        throw new IOException("b64 must be canonical");
                    }
                } catch (IllegalArgumentException malformed) {
                    throw new IOException("b64 must be unpadded base64url", malformed);
                }
                return value;
            }
        }
    }

    private static <T> T required(T value, String name) {
        return Objects.requireNonNull(value, name + " is required");
    }

    /** One STREAM connection of the reporting node, identified by stream name and SessionRid. */
    public record ConnectionRef(
            String streamName, @JsonDeserialize(using = Rid.Reader.class) String sessionRid) {
        public ConnectionRef {
            required(streamName, "streamName");
            required(sessionRid, "sessionRid");
        }
    }

    /** Connection identity (node RID, node lifecycle generation, STREAM name, SessionRid). */
    public record Connection(
            @JsonDeserialize(using = Rid.Reader.class) String nodeRid,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long nodeLifecycleGeneration,
            String streamName,
            @JsonDeserialize(using = Rid.Reader.class) String sessionRid) {
        public Connection {
            required(nodeRid, "nodeRid");
            required(nodeLifecycleGeneration, "nodeLifecycleGeneration");
            required(streamName, "streamName");
            required(sessionRid, "sessionRid");
        }
    }

    /** A position update or the ticket for one waiting connection of the receiving node. */
    @JsonInclude(JsonInclude.Include.NON_NULL)
    public record Delivery(
            String streamName,
            @JsonDeserialize(using = Rid.Reader.class) String sessionRid,
            String kind,
            @ZLinkJsonOptional @JsonInclude(JsonInclude.Include.NON_NULL)
                    @Nullable Integer position,
            @ZLinkJsonOptional @JsonInclude(JsonInclude.Include.NON_NULL)
                    @Nullable Integer estimatedWaitMs,
            @ZLinkJsonOptional @JsonInclude(JsonInclude.Include.NON_NULL) @Nullable String ticket,
            @ZLinkJsonOptional @JsonInclude(JsonInclude.Include.NON_NULL)
                    @Nullable Integer expiresInMs) {
        public static final String POSITION = "position";
        public static final String TICKET = "ticket";

        public Delivery {
            required(streamName, "streamName");
            required(sessionRid, "sessionRid");
            required(kind, "kind");
            if (kind.equals(POSITION)) {
                required(position, "position");
                required(estimatedWaitMs, "estimatedWaitMs");
            } else if (kind.equals(TICKET)) {
                required(ticket, "ticket");
                required(expiresInMs, "expiresInMs");
                if (ticket.length() != 66 || !B64.matcher(ticket).matches()) {
                    throw new IllegalArgumentException("ticket must be 66 base64url characters");
                }
            } else {
                throw new IllegalArgumentException("kind must be position or ticket");
            }
        }
    }

    @ZLinkPacket("$zlink.admission-queue.report")
    public record NodeReport(
            @JsonDeserialize(using = Rid.Reader.class) String nodeRid,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long nodeLifecycleGeneration,
            Integer limit,
            Integer inFlight,
            List<ConnectionRef> usedTickets,
            List<ConnectionRef> endedSessions,
            @JsonSerialize(contentUsing = Dec.Writer.class)
                    @JsonDeserialize(contentUsing = Dec.Reader.class)
                    List<Long> heldPermits,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long resolvedRequestSeq) {
        public NodeReport {
            required(nodeRid, "nodeRid");
            required(nodeLifecycleGeneration, "nodeLifecycleGeneration");
            required(limit, "limit");
            required(inFlight, "inFlight");
            required(usedTickets, "usedTickets");
            required(endedSessions, "endedSessions");
            required(heldPermits, "heldPermits");
            required(resolvedRequestSeq, "resolvedRequestSeq");
        }
    }

    @ZLinkPacket("$zlink.admission-queue.report.reply")
    public record NodeReportReply(
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long epoch,
            List<Delivery> deliveries) {
        public NodeReportReply {
            required(epoch, "epoch");
            required(deliveries, "deliveries");
        }
    }

    @ZLinkPacket("$zlink.admission-queue.enqueue")
    public record Enqueue(Connection connection) {
        public Enqueue {
            required(connection, "connection");
        }
    }

    @ZLinkPacket("$zlink.admission-queue.enqueue.reply")
    public record EnqueueReply(
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long epoch,
            Delivery delivery) {
        public EnqueueReply {
            required(epoch, "epoch");
            required(delivery, "delivery");
        }
    }

    @ZLinkPacket("$zlink.admission-queue.admit-permit")
    public record AdmitPermit(
            @JsonDeserialize(using = Rid.Reader.class) String nodeRid,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long nodeLifecycleGeneration,
            String ownerId,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long leaseGeneration,
            String streamName,
            @JsonDeserialize(using = Rid.Reader.class) String sessionRid,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long epoch,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long requestSeq) {
        public AdmitPermit {
            required(nodeRid, "nodeRid");
            required(nodeLifecycleGeneration, "nodeLifecycleGeneration");
            required(ownerId, "ownerId");
            required(leaseGeneration, "leaseGeneration");
            required(streamName, "streamName");
            required(sessionRid, "sessionRid");
            required(epoch, "epoch");
            required(requestSeq, "requestSeq");
        }
    }

    @JsonInclude(JsonInclude.Include.NON_NULL)
    @ZLinkPacket("$zlink.admission-queue.admit-permit.reply")
    public record AdmitPermitReply(
            Boolean granted,
            @ZLinkJsonOptional
                    @JsonInclude(JsonInclude.Include.NON_NULL)
                    @JsonSerialize(using = Dec.Writer.class)
                    @JsonDeserialize(using = Dec.Reader.class)
                    @Nullable Long permitId) {
        public AdmitPermitReply {
            required(granted, "granted");
            if (granted && (permitId == null || permitId == 0)) {
                throw new IllegalArgumentException("a granted permit needs a non-zero permitId");
            }
        }
    }

    @ZLinkPacket("$zlink.admission-queue.release-permit")
    public record ReleasePermit(
            @JsonDeserialize(using = Rid.Reader.class) String nodeRid,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long nodeLifecycleGeneration,
            @JsonSerialize(using = Dec.Writer.class) @JsonDeserialize(using = Dec.Reader.class)
                    Long permitId) {
        public ReleasePermit {
            required(nodeRid, "nodeRid");
            required(nodeLifecycleGeneration, "nodeLifecycleGeneration");
            required(permitId, "permitId");
        }
    }
}
