package MyGame;

import com.fasterxml.jackson.annotation.JsonIgnore;

import java.util.List;
import java.util.Optional;

public final class Geometry {
    private Geometry() {}

    public record Vec3(double x, double y, double z) {
        @JsonIgnore
        public boolean isFinite() {
            return Double.isFinite(x) && Double.isFinite(y) && Double.isFinite(z);
        }

        public double length() {
            double scale = Math.max(Math.abs(x), Math.max(Math.abs(y), Math.abs(z)));
            if (scale == 0 || !Double.isFinite(scale)) return scale;
            double sx = x / scale, sy = y / scale, sz = z / scale;
            return scale * Math.sqrt(sx * sx + sy * sy + sz * sz);
        }

        public Optional<Vec3> tryNormalize() {
            double scale = Math.max(Math.abs(x), Math.max(Math.abs(y), Math.abs(z)));
            if (!isFinite() || scale == 0) return Optional.empty();
            var scaled = new Vec3(x / scale, y / scale, z / scale);
            return Optional.of(scaled.times(1 / scaled.length()));
        }

        public Vec3 plus(Vec3 other) {
            return new Vec3(x + other.x, y + other.y, z + other.z);
        }

        public Vec3 minus(Vec3 other) {
            return new Vec3(x - other.x, y - other.y, z - other.z);
        }

        public Vec3 times(double scale) {
            return new Vec3(x * scale, y * scale, z * scale);
        }
    }

    public record Aabb(Vec3 min, Vec3 max) {
        public boolean contains(Vec3 point) {
            return point.x() >= min.x() && point.x() <= max.x()
                    && point.y() >= min.y() && point.y() <= max.y()
                    && point.z() >= min.z() && point.z() <= max.z();
        }

        public Double intersect(Vec3 origin, Vec3 direction, double limit) {
            double[] interval = {0, limit};
            if (!slab(origin.x(), direction.x(), min.x(), max.x(), interval)
                    || !slab(origin.y(), direction.y(), min.y(), max.y(), interval)
                    || !slab(origin.z(), direction.z(), min.z(), max.z(), interval))
                return null;
            return interval[0];
        }

        private static boolean slab(
                double origin, double direction, double min, double max, double[] interval) {
            if (direction == 0) return origin >= min && origin <= max;
            double a = (min - origin) / direction;
            double b = (max - origin) / direction;
            interval[0] = Math.max(interval[0], Math.min(a, b));
            interval[1] = Math.min(interval[1], Math.max(a, b));
            return interval[0] <= interval[1];
        }
    }

    public static final class ArenaMap {
        private ArenaMap() {}

        public static final Aabb BOUNDS =
                new Aabb(new Vec3(0, 0, 0), new Vec3(100, 10, 100));
        public static final List<Aabb> WALLS =
                List.of(
                        new Aabb(new Vec3(49, 0, 20), new Vec3(51, 10, 60)),
                        new Aabb(new Vec3(80, 0, 0), new Vec3(82, 10, 20)));
        public static final List<Vec3> SPAWNS =
                List.of(new Vec3(48, 0, 18), new Vec3(52, 0, 18));

        public static Aabb playerBox(Vec3 feet) {
            return new Aabb(
                    feet.plus(new Vec3(-Rules.PLAYER_HALF_WIDTH, 0, -Rules.PLAYER_HALF_WIDTH)),
                    feet.plus(
                            new Vec3(
                                    Rules.PLAYER_HALF_WIDTH,
                                    Rules.PLAYER_HEIGHT,
                                    Rules.PLAYER_HALF_WIDTH)));
        }
    }
}
