package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.module.SimpleModule;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;
import systems.zlink.framework.spots.SpotRef;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;

public final class ZLinkJsonMessageSerializer implements ZLinkMessageSerializer {
    private static final String FIELD_ACTOR_ID = "actorId";
    private static final String FIELD_OBJECT_GENERATION = "objectGeneration";
    private static final String FIELD_MESH_NAME = "meshName";
    private static final String FIELD_NODE_RID = "nodeRid";
    private static final String FIELD_SPOT_ID = "spotId";
    private final ObjectMapper mapper;

    public ZLinkJsonMessageSerializer() {
        this(ZLinkFrameworkJsonProfile.mapper(actorRefModule(), spotRefModule()));
    }

    ZLinkJsonMessageSerializer(ObjectMapper mapper) {
        this.mapper = mapper;
    }

    @Override
    public <T> ZLinkEncodedPayload serialize(T value) {
        if (value instanceof Message message) {
            return ZLinkEncodedPayload.from(message.toByteArray());
        }
        if (value instanceof byte[] bytes) {
            return ZLinkEncodedPayload.from(bytes);
        }
        try {
            return ZLinkEncodedPayload.from(mapper.writeValueAsBytes(value));
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException(
                    "failed to serialize message as JSON: " + valueTypeName(value), ex);
        }
    }

    Message serializeOwned(Object value) {
        if (value instanceof Message message) {
            return Message.from(message);
        }
        if (value instanceof byte[] bytes) {
            return Message.from(bytes);
        }
        try {
            return Message.from(mapper.writeValueAsBytes(value));
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException(
                    "failed to serialize message as JSON: " + valueTypeName(value), ex);
        }
    }

    @Override
    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
        byte[] bytes = payload.bytes();
        if (type == Message.class) {
            return type.cast(Message.from(bytes));
        }
        if (type == byte[].class) {
            return type.cast(bytes);
        }
        try {
            return mapper.readValue(bytes, type);
        } catch (IOException ex) {
            throw new IllegalArgumentException(
                    "failed to deserialize JSON message as "
                            + type.getName()
                            + " payload="
                            + new String(bytes, StandardCharsets.UTF_8),
                    ex);
        }
    }

    @Override
    public void prepare(Class<?> type) {
        if (type == null || type == Void.class || type == Message.class || type == byte[].class) {
            return;
        }
        mapper.canSerialize(type);
        mapper.canDeserialize(mapper.constructType(type));
    }

    private static String valueTypeName(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }

    private static SimpleModule actorRefModule() {
        SimpleModule module = new SimpleModule("zlink-actor-ref");
        module.addSerializer(
                ActorRef.class,
                new JsonSerializer<>() {
                    @Override
                    public void serialize(
                            ActorRef value, JsonGenerator generator, SerializerProvider serializers)
                            throws IOException {
                        generator.writeStartObject();
                        generator.writeStringField(FIELD_ACTOR_ID, value.actorId());
                        generator.writeStringField(
                                FIELD_OBJECT_GENERATION,
                                Long.toUnsignedString(value.objectGeneration()));
                        generator.writeStringField(FIELD_MESH_NAME, value.meshName());
                        generator.writeStringField(FIELD_NODE_RID, value.nodeRid().toHex());
                        generator.writeEndObject();
                    }
                });
        module.addDeserializer(
                ActorRef.class,
                new JsonDeserializer<>() {
                    @Override
                    public ActorRef deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        if (parser.currentToken() != JsonToken.START_OBJECT) {
                            throw JsonMappingException.from(
                                    parser, "ActorRef must be a JSON object");
                        }
                        String actorId = null;
                        String objectGeneration = null;
                        String meshName = null;
                        String nodeRid = null;
                        Set<String> seen = new HashSet<>();
                        while (parser.nextToken() != JsonToken.END_OBJECT) {
                            String field = parser.currentName();
                            parser.nextToken();
                            if (!seen.add(field)) {
                                throw context.weirdStringException(
                                        field, ActorRef.class, "duplicate ActorRef property");
                            }
                            switch (field) {
                                case FIELD_ACTOR_ID ->
                                        actorId = requireString(parser, context, field);
                                case FIELD_OBJECT_GENERATION ->
                                        objectGeneration = requireString(parser, context, field);
                                case FIELD_MESH_NAME ->
                                        meshName = requireString(parser, context, field);
                                case FIELD_NODE_RID ->
                                        nodeRid = requireString(parser, context, field);
                                default ->
                                        throw JsonMappingException.from(
                                                parser, "unknown ActorRef property: " + field);
                            }
                        }
                        if (actorId == null
                                || objectGeneration == null
                                || meshName == null
                                || nodeRid == null
                                || !objectGeneration.matches("[1-9][0-9]*")) {
                            throw JsonMappingException.from(
                                    parser,
                                    "ActorRef requires actorId, positive decimal-string "
                                            + "objectGeneration, meshName and nodeRid");
                        }
                        try {
                            return new ActorRef(
                                    actorId,
                                    Long.parseUnsignedLong(objectGeneration),
                                    meshName,
                                    RoutingId.fromHex(nodeRid));
                        } catch (RuntimeException failure) {
                            throw context.weirdStringException(
                                    objectGeneration, ActorRef.class, "invalid ActorRef");
                        }
                    }

                    private String requireString(
                            JsonParser parser, DeserializationContext context, String field)
                            throws IOException {
                        if (parser.currentToken() != JsonToken.VALUE_STRING) {
                            throw JsonMappingException.from(
                                    parser, "ActorRef " + field + " must be a string");
                        }
                        return parser.getText();
                    }
                });
        return module;
    }

    private static SimpleModule spotRefModule() {
        SimpleModule module = new SimpleModule("zlink-spot-ref");
        module.addSerializer(
                SpotRef.class,
                new JsonSerializer<>() {
                    @Override
                    public void serialize(
                            SpotRef value, JsonGenerator generator, SerializerProvider serializers)
                            throws IOException {
                        generator.writeStartObject();
                        generator.writeStringField(FIELD_SPOT_ID, value.spotId());
                        generator.writeStringField(
                                FIELD_OBJECT_GENERATION,
                                Long.toUnsignedString(value.objectGeneration()));
                        generator.writeStringField(FIELD_MESH_NAME, value.meshName());
                        generator.writeStringField(FIELD_NODE_RID, value.nodeRid().toHex());
                        generator.writeEndObject();
                    }
                });
        module.addDeserializer(
                SpotRef.class,
                new JsonDeserializer<>() {
                    @Override
                    public SpotRef deserialize(JsonParser parser, DeserializationContext context)
                            throws IOException {
                        if (parser.currentToken() != JsonToken.START_OBJECT) {
                            throw JsonMappingException.from(
                                    parser, "SpotRef must be a JSON object");
                        }
                        String spotId = null;
                        String objectGeneration = null;
                        String meshName = null;
                        String nodeRid = null;
                        Set<String> seen = new HashSet<>();
                        while (parser.nextToken() != JsonToken.END_OBJECT) {
                            String field = parser.currentName();
                            parser.nextToken();
                            if (!seen.add(field)) {
                                throw context.weirdStringException(
                                        field, SpotRef.class, "duplicate SpotRef property");
                            }
                            switch (field) {
                                case FIELD_SPOT_ID ->
                                        spotId = requireRefString(parser, field, "SpotRef");
                                case FIELD_OBJECT_GENERATION ->
                                        objectGeneration =
                                                requireRefString(parser, field, "SpotRef");
                                case FIELD_MESH_NAME ->
                                        meshName = requireRefString(parser, field, "SpotRef");
                                case FIELD_NODE_RID ->
                                        nodeRid = requireRefString(parser, field, "SpotRef");
                                default ->
                                        throw JsonMappingException.from(
                                                parser, "unknown SpotRef property: " + field);
                            }
                        }
                        if (spotId == null
                                || objectGeneration == null
                                || meshName == null
                                || nodeRid == null
                                || !objectGeneration.matches("[1-9][0-9]*")) {
                            throw JsonMappingException.from(
                                    parser,
                                    "SpotRef requires spotId, positive decimal-string "
                                            + "objectGeneration, meshName and nodeRid");
                        }
                        try {
                            return new SpotRef(
                                    spotId,
                                    Long.parseUnsignedLong(objectGeneration),
                                    meshName,
                                    RoutingId.fromHex(nodeRid));
                        } catch (RuntimeException failure) {
                            throw context.weirdStringException(
                                    objectGeneration, SpotRef.class, "invalid SpotRef");
                        }
                    }
                });
        return module;
    }

    private static String requireRefString(JsonParser parser, String field, String typeName)
            throws IOException {
        if (parser.currentToken() != JsonToken.VALUE_STRING) {
            throw JsonMappingException.from(parser, typeName + " " + field + " must be a string");
        }
        return parser.getText();
    }
}
