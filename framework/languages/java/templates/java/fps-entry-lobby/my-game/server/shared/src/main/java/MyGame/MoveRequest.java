package MyGame;

import MyGame.Geometry.Vec3;

public record MoveRequest(Vec3 delta, long sequence) {}
