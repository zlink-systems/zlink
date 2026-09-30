package systems.zlink.framework.perf;

import java.nio.file.Path;

// The cell directory (§15.1): role-configs/<role>.json sits one folder below it; the PS sequence originals are written there.
public record CellDirectory(Path path) {}
