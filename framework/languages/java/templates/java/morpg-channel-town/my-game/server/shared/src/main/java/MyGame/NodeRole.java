package MyGame;

public enum NodeRole {
  EXTERNAL("external"),
  SESSION("session"),
  TOWN("town"),
  INSTANCE("instance"),
  LOCATION_STORE("location-store");
  private final String id;

  NodeRole(String id) {
    this.id = id;
  }

  public String id() {
    return id;
  }

  public boolean application() {
    return this == SESSION || this == TOWN || this == INSTANCE;
  }

  public static NodeRole parse(String id) {
    for (var role : values()) if (role.id.equals(id)) return role;
    throw new IllegalArgumentException("Unknown node role: " + id);
  }
}
