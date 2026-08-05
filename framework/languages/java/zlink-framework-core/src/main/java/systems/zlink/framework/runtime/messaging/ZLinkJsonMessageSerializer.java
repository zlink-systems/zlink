package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializerProvider;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.module.SimpleModule;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.spots.SpotRef;

public final class ZLinkJsonMessageSerializer implements ZLinkMessageSerializer {
    private final ObjectMapper mapper;

    public ZLinkJsonMessageSerializer() {
        this(JsonMapper.builder()
            .configure(MapperFeature.ACCEPT_CASE_INSENSITIVE_PROPERTIES, true)
            .configure(MapperFeature.USE_STD_BEAN_NAMING, true)
            .findAndAddModules()
            .addModule(routingIdModule())
            .addModule(actorRefModule())
            .addModule(spotRefModule())
            .build());
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
                "failed to serialize message as JSON: " + valueTypeName(value),
                ex);
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
                    + new String(bytes, java.nio.charset.StandardCharsets.UTF_8),
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

    private static SimpleModule routingIdModule() {
        SimpleModule module = new SimpleModule("zlink-routing-id");
        module.addSerializer(RoutingId.class, new JsonSerializer<>() {
            @Override
            public void serialize(
                RoutingId value,
                JsonGenerator generator,
                SerializerProvider serializers) throws IOException {
                generator.writeString(value.toHex());
            }
        });
        module.addDeserializer(RoutingId.class, new JsonDeserializer<>() {
            @Override
            public RoutingId deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                return RoutingId.fromHex(parser.getValueAsString());
            }
        });
        return module;
    }

    private static SimpleModule actorRefModule() {
        SimpleModule module = new SimpleModule("zlink-actor-ref");
        module.addSerializer(ActorRef.class, new JsonSerializer<>() {
            @Override
            public void serialize(
                ActorRef value,
                JsonGenerator generator,
                SerializerProvider serializers) throws IOException {
                generator.writeStartObject();
                generator.writeStringField("actorId", value.actorId());
                generator.writeStringField(
                    "objectGeneration",
                    Long.toString(value.objectGeneration()));
                generator.writeStringField("meshName", value.meshName());
                generator.writeStringField(
                    "nodeRid",
                    value.nodeRid().toHex());
                generator.writeEndObject();
            }
        });
        module.addDeserializer(ActorRef.class, new JsonDeserializer<>() {
            @Override
            public ActorRef deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                if (parser.currentToken() != JsonToken.START_OBJECT) {
                    throw JsonMappingException.from(parser,
                        "ActorRef must be a JSON object");
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
                            field,
                            ActorRef.class,
                            "duplicate ActorRef property");
                    }
                    switch (field) {
                        case "actorId" -> actorId = requireString(
                            parser, context, field);
                        case "objectGeneration" -> objectGeneration =
                            requireString(parser, context, field);
                        case "meshName" -> meshName = requireString(
                            parser, context, field);
                        case "nodeRid" -> nodeRid = requireString(
                            parser, context, field);
                        default -> throw JsonMappingException.from(parser,
                            "unknown ActorRef property: " + field);
                    }
                }
                if (actorId == null || objectGeneration == null
                    || meshName == null || nodeRid == null
                    || !objectGeneration.matches("[1-9][0-9]*")) {
                    throw JsonMappingException.from(parser,
                        "ActorRef requires actorId, positive decimal-string "
                            + "objectGeneration, meshName and nodeRid");
                }
                try {
                    return new ActorRef(
                        actorId,
                        Long.parseLong(objectGeneration),
                        meshName,
                        RoutingId.fromHex(nodeRid));
                } catch (RuntimeException failure) {
                    throw context.weirdStringException(
                        objectGeneration,
                        ActorRef.class,
                        "invalid ActorRef");
                }
            }

            private String requireString(
                JsonParser parser,
                DeserializationContext context,
                String field) throws IOException {
                if (parser.currentToken() != JsonToken.VALUE_STRING) {
                    throw JsonMappingException.from(parser,
                        "ActorRef " + field + " must be a string");
                }
                return parser.getText();
            }
        });
        return module;
    }

    private static SimpleModule spotRefModule() {
        SimpleModule module = new SimpleModule("zlink-spot-ref");
        module.addSerializer(SpotRef.class, new JsonSerializer<>() {
            @Override
            public void serialize(
                SpotRef value,
                JsonGenerator generator,
                SerializerProvider serializers) throws IOException {
                generator.writeStartObject();
                generator.writeStringField("spotId", value.spotId());
                generator.writeStringField(
                    "objectGeneration",
                    Long.toString(value.objectGeneration()));
                generator.writeStringField("meshName", value.meshName());
                generator.writeStringField("nodeRid", value.nodeRid().toHex());
                generator.writeEndObject();
            }
        });
        module.addDeserializer(SpotRef.class, new JsonDeserializer<>() {
            @Override
            public SpotRef deserialize(
                JsonParser parser,
                DeserializationContext context) throws IOException {
                if (parser.currentToken() != JsonToken.START_OBJECT) {
                    throw JsonMappingException.from(parser,
                        "SpotRef must be a JSON object");
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
                            field,
                            SpotRef.class,
                            "duplicate SpotRef property");
                    }
                    switch (field) {
                        case "spotId" -> spotId = requireRefString(
                            parser, field, "SpotRef");
                        case "objectGeneration" -> objectGeneration =
                            requireRefString(parser, field, "SpotRef");
                        case "meshName" -> meshName = requireRefString(
                            parser, field, "SpotRef");
                        case "nodeRid" -> nodeRid = requireRefString(
                            parser, field, "SpotRef");
                        default -> throw JsonMappingException.from(parser,
                            "unknown SpotRef property: " + field);
                    }
                }
                if (spotId == null || objectGeneration == null
                    || meshName == null || nodeRid == null
                    || !objectGeneration.matches("[1-9][0-9]*")) {
                    throw JsonMappingException.from(parser,
                        "SpotRef requires spotId, positive decimal-string "
                            + "objectGeneration, meshName and nodeRid");
                }
                try {
                    return new SpotRef(
                        spotId,
                        Long.parseLong(objectGeneration),
                        meshName,
                        RoutingId.fromHex(nodeRid));
                } catch (RuntimeException failure) {
                    throw context.weirdStringException(
                        objectGeneration,
                        SpotRef.class,
                        "invalid SpotRef");
                }
            }
        });
        return module;
    }

    private static String requireRefString(
        JsonParser parser,
        String field,
        String typeName) throws IOException {
        if (parser.currentToken() != JsonToken.VALUE_STRING) {
            throw JsonMappingException.from(parser,
                typeName + " " + field + " must be a string");
        }
        return parser.getText();
    }
}
