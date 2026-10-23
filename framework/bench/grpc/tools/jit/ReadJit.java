import java.nio.file.Path;
import java.time.Instant;
import java.util.HashMap;
import java.util.Map;
import jdk.jfr.consumer.RecordedEvent;
import jdk.jfr.consumer.RecordedMethod;
import jdk.jfr.consumer.RecordingFile;

/** Streams compilation evidence; a terminal record distinguishes complete output from truncation. */
public final class ReadJit {
  private static String quote(String value) {
    if (value == null) return "null";
    StringBuilder result = new StringBuilder("\"");
    for (int i = 0; i < value.length(); i++) {
      char ch = value.charAt(i);
      if (ch == '\\' || ch == '"') result.append('\\').append(ch);
      else if (ch < 0x20) result.append(String.format("\\u%04x", (int) ch));
      else result.append(ch);
    }
    return result.append('"').toString();
  }

  private static long nanos(Instant instant) {
    return Math.addExact(Math.multiplyExact(instant.getEpochSecond(), 1_000_000_000L), instant.getNano());
  }

  private static String method(RecordedEvent event) {
    RecordedMethod method = event.getValue("method");
    return method == null
        ? "<null>"
        : method.getType().getName() + "." + method.getName() + method.getDescriptor();
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 1) throw new IllegalArgumentException("Usage: ReadJit <recording.jfr>");
    Path path = Path.of(args[0]);
    Map<Long, String> eventNames = new HashMap<>();
    try (RecordingFile recording = new RecordingFile(path)) {
      for (var type : recording.readEventTypes()) eventNames.put(type.getId(), type.getName());
    }
    long emitted = 0;
    try (RecordingFile recording = new RecordingFile(path)) {
      while (recording.hasMoreEvents()) {
        RecordedEvent event = recording.readEvent();
        String type = event.getEventType().getName();
        String row;
        switch (type) {
          case "measure.MonotonicAnchor" ->
              row = "{\"type\":\"anchor\",\"timestamp_ns\":" + nanos(event.getStartTime())
                  + ",\"lower_ns\":" + event.getLong("lowerNs")
                  + ",\"upper_ns\":" + event.getLong("upperNs") + "}";
          case "measure.Phase" ->
              row = "{\"type\":\"phase\",\"phase\":" + quote(event.getString("phase"))
                  + ",\"start_ns\":" + event.getLong("startNs")
                  + ",\"duration_ns\":" + event.getLong("durationNs")
                  + ",\"cell_id\":" + quote(event.hasField("cellId") ? event.getString("cellId") : "") + "}";
          case "jdk.Compilation" ->
              row = "{\"type\":\"compilation\",\"timestamp_ns\":" + nanos(event.getStartTime())
                  + ",\"duration_ns\":" + event.getDuration().toNanos()
                  + ",\"compile_id\":" + event.getInt("compileId")
                  + ",\"compiler\":" + quote(event.getString("compiler"))
                  + ",\"method\":" + quote(method(event))
                  + ",\"level\":" + event.getInt("compileLevel")
                  + ",\"osr\":" + event.getBoolean("isOsr")
                  + ",\"succeeded\":" + event.getBoolean("succeded") + "}";
          case "jdk.Deoptimization" ->
              row = "{\"type\":\"deoptimization\",\"timestamp_ns\":" + nanos(event.getStartTime())
                  + ",\"compile_id\":" + event.getInt("compileId")
                  + ",\"method\":" + quote(method(event))
                  + ",\"bci\":" + event.getInt("bci")
                  + ",\"reason\":" + quote(event.getString("reason"))
                  + ",\"action\":" + quote(event.getString("action")) + "}";
          case "jdk.DataLoss" -> row = "{\"type\":\"data_loss\"}";
          case "jdk.ActiveSetting" -> {
            String owner = eventNames.get(event.getLong("id"));
            if (!"jdk.Compilation".equals(owner) && !"jdk.DataLoss".equals(owner)
                && !"measure.MonotonicAnchor".equals(owner) && !"measure.Phase".equals(owner)) continue;
            row = "{\"type\":\"setting\",\"event\":" + quote(owner)
                + ",\"name\":" + quote(event.getString("name"))
                + ",\"value\":" + quote(event.getString("value")) + "}";
          }
          default -> { continue; }
        }
        System.out.println(row);
        emitted++;
      }
    }
    System.out.println("{\"type\":\"end\",\"records\":" + emitted + "}");
    if (System.out.checkError()) throw new java.io.IOException("Reader output could not be written");
  }
}
