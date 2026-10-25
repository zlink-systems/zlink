using System.Text.Json.Serialization;

namespace MyGame.Shared;

public readonly record struct Vec3(
    [property: JsonNumberHandling(JsonNumberHandling.AllowNamedFloatingPointLiterals)] double X,
    [property: JsonNumberHandling(JsonNumberHandling.AllowNamedFloatingPointLiterals)] double Y,
    [property: JsonNumberHandling(JsonNumberHandling.AllowNamedFloatingPointLiterals)] double Z)
{
    [JsonIgnore]
    public bool IsFinite => double.IsFinite(X) && double.IsFinite(Y) && double.IsFinite(Z);
    [JsonIgnore]
    public double Length
    {
        get
        {
            var scale = Math.Max(Math.Abs(X), Math.Max(Math.Abs(Y), Math.Abs(Z)));
            if (scale == 0 || !double.IsFinite(scale))
                return scale;
            var x = X / scale;
            var y = Y / scale;
            var z = Z / scale;
            return scale * Math.Sqrt(x * x + y * y + z * z);
        }
    }
    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator *(Vec3 a, double scale) => new(a.X * scale, a.Y * scale, a.Z * scale);

    public bool TryNormalize(out Vec3 direction)
    {
        var scale = Math.Max(Math.Abs(X), Math.Max(Math.Abs(Y), Math.Abs(Z)));
        if (!IsFinite || scale == 0)
        {
            direction = default;
            return false;
        }
        var scaled = new Vec3(X / scale, Y / scale, Z / scale);
        direction = scaled * (1 / scaled.Length);
        return true;
    }
}

public readonly record struct Aabb(Vec3 Min, Vec3 Max)
{
    public bool Contains(Vec3 point) =>
        point.X >= Min.X && point.X <= Max.X && point.Y >= Min.Y && point.Y <= Max.Y
        && point.Z >= Min.Z && point.Z <= Max.Z;

    // The closed slab interval includes faces, edges, corners, and origins inside the box.
    public double? Intersect(Vec3 origin, Vec3 direction, double limit)
    {
        var near = 0.0;
        var far = limit;
        if (!Slab(origin.X, direction.X, Min.X, Max.X, ref near, ref far)
            || !Slab(origin.Y, direction.Y, Min.Y, Max.Y, ref near, ref far)
            || !Slab(origin.Z, direction.Z, Min.Z, Max.Z, ref near, ref far))
            return null;
        return near;
    }

    private static bool Slab(double origin, double direction, double min, double max,
        ref double near, ref double far)
    {
        if (direction == 0)
            return origin >= min && origin <= max;
        var a = (min - origin) / direction;
        var b = (max - origin) / direction;
        near = Math.Max(near, Math.Min(a, b));
        far = Math.Min(far, Math.Max(a, b));
        return near <= far;
    }
}

public static class ArenaMap
{
    public static readonly Aabb Bounds = new(new(0, 0, 0), new(100, 10, 100));
    public static IReadOnlyList<Aabb> Walls { get; } = Array.AsReadOnly(new Aabb[]
    {
        new(new(49, 0, 20), new(51, 10, 60)),
        new(new(80, 0, 0), new(82, 10, 20)),
    });
    public static IReadOnlyList<Vec3> Spawns { get; } = Array.AsReadOnly(new Vec3[]
    {
        new(48, 0, 18), new(52, 0, 18),
    });

    public static Aabb PlayerBox(Vec3 feet) => new(
        feet + new Vec3(-GameNames.PlayerHalfWidth, 0, -GameNames.PlayerHalfWidth),
        feet + new Vec3(GameNames.PlayerHalfWidth, GameNames.PlayerHeight, GameNames.PlayerHalfWidth));
}
