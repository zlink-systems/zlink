extends SceneTree

func _init() -> void:
    var missing_sdk := true
    if missing_sdk:
        print("engine-required: Godot runtime gate requires Godot 4.x and godot-cpp build artifacts")
        quit(0)
        return
    quit(1)
