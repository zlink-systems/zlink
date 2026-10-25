package MyGame;

import MyGame.Geometry.Vec3;

public record ShootRequest(Vec3 aimDirection, long sequence) {}
