package systems.zlink.crosslanguage.host;

import java.time.Instant;

record ActorDestroyEvents(EventSink sink, String runId) {
    void emit(String event, Object... fields) {
        emitAt(Instant.now(), event, fields);
    }

    synchronized void emitAt(Instant now, String event, Object... fields) {
        long micros = now.getEpochSecond() * 1_000_000 + now.getNano() / 1_000;
        var line =
                new StringBuilder("actor-destroy|run=")
                        .append(escape(runId))
                        .append("|event=")
                        .append(event)
                        .append("|timeUs=")
                        .append(micros);
        for (int index = 0; index < fields.length; index += 2) {
            line.append('|').append(fields[index]).append('=').append(escape(fields[index + 1]));
        }
        sink.append(line.toString());
    }

    private static String escape(Object value) {
        return String.valueOf(value)
                .replace("%", "%25")
                .replace("|", "%7C")
                .replace("\r", "%0D")
                .replace("\n", "%0A");
    }
}
