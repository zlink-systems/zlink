import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Path;
import java.time.Instant;
import java.util.HashMap;
import java.util.Map;
import jdk.jfr.consumer.RecordedEvent;
import jdk.jfr.consumer.RecordedMethod;
import jdk.jfr.consumer.RecordingFile;

/** Streams compilation evidence; a terminal record distinguishes complete output from truncation. */
public final class ReadJit {
  /** Validates every completed chunk before the consumer can silently accept a prefix. */
  private static String recordingBounds(Path path) throws IOException {
    final int headerBytes = 68;
    long offset = 0, firstStart = -1, coverageEnd = -1, previousStart = -1;
    int chunks = 0;
    boolean finalChunk = false;
    StringBuilder headers = new StringBuilder("[");
    try (RandomAccessFile file = new RandomAccessFile(path.toFile(), "r")) {
      long fileSize = file.length();
      while (offset < fileSize) {
        if (fileSize - offset < headerBytes) throw new IOException("Truncated JFR chunk header");
        file.seek(offset);
        if (file.readInt() != 0x464c5200) throw new IOException("Invalid JFR chunk magic");
        int major = file.readUnsignedShort(), minor = file.readUnsignedShort();
        if (major != 1 && major != 2) throw new IOException("Unsupported JFR major version: " + major);
        long size = file.readLong(), constantPool = file.readLong(), metadata = file.readLong();
        long start = file.readLong(), duration = file.readLong();
        file.readLong(); // startTicks
        long ticksPerSecond = file.readLong();
        int state = file.readUnsignedByte();
        file.skipBytes(2);
        int flags = file.readUnsignedByte();
        if (size < headerBytes || size > fileSize - offset) throw new IOException("Invalid JFR chunk size");
        if ((constantPool != 0 && (constantPool < headerBytes || constantPool >= size))
            || (metadata != 0 && (metadata < headerBytes || metadata >= size))) {
          throw new IOException("JFR checkpoint or metadata is outside its chunk");
        }
        if (state != 0) throw new IOException("JFR chunk is not finished");
        if (start <= 0 || duration < 0 || ticksPerSecond <= 0 || duration > Long.MAX_VALUE - start) {
          throw new IOException("Invalid JFR chunk time bounds");
        }
        long end = start + duration;
        if (chunks > 0 && (start < previousStart || start > coverageEnd)) {
          throw new IOException("JFR chunk coverage has a gap or reversed start time");
        }
        finalChunk = (flags & 2) != 0;
        if (finalChunk && offset + size != fileSize) throw new IOException("Final JFR chunk is not last");
        if (chunks == 0) firstStart = start;
        coverageEnd = Math.max(coverageEnd, end);
        previousStart = start;
        if (chunks > 0) headers.append(',');
        headers.append("{\"offset\":").append(offset).append(",\"size\":").append(size)
            .append(",\"major\":").append(major).append(",\"minor\":").append(minor)
            .append(",\"start_epoch_ns\":").append(start).append(",\"end_epoch_ns\":").append(end)
            .append(",\"state\":").append(state).append(",\"flags\":").append(flags).append('}');
        offset += size;
        chunks++;
      }
      if (chunks == 0 || offset != fileSize) throw new IOException("Missing complete JFR chunks");
      return "{\"type\":\"recording\",\"start_epoch_ns\":" + firstStart
          + ",\"end_epoch_ns\":" + coverageEnd + ",\"final_chunk\":" + finalChunk
          + ",\"file_size\":" + fileSize + ",\"chunks\":" + headers.append(']') + "}";
    }
  }

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
    String bounds = recordingBounds(path);
    Map<Long, String> eventNames = new HashMap<>();
    try (RecordingFile recording = new RecordingFile(path)) {
      for (var type : recording.readEventTypes()) eventNames.put(type.getId(), type.getName());
    }
    System.out.println(bounds);
    long emitted = 1;
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
