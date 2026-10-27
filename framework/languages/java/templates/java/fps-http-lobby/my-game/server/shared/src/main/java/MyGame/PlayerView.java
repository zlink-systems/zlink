package MyGame;

import MyGame.Geometry.Vec3;

public record PlayerView(String playerId, Vec3 position, int hp, long lastSequence) {}
