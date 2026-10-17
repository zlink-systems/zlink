package MyGame;

import com.fasterxml.jackson.databind.ObjectMapper;

public final class Json {
    private Json() {}

    public static final ObjectMapper MAPPER = new ObjectMapper().findAndRegisterModules();
}
