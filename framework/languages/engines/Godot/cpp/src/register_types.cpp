#include "engine_lobby_node.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_engine_lobby (ModuleInitializationLevel level)
{
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        ClassDB::register_class<EngineLobbyNode> ();
    }
}

void uninitialize_engine_lobby (ModuleInitializationLevel)
{
}

extern "C" GDExtensionBool GDE_EXPORT
engine_lobby_library_init (GDExtensionInterfaceGetProcAddress get_proc_address,
                           const GDExtensionClassLibraryPtr library,
                           GDExtensionInitialization *initialization)
{
    GDExtensionBinding::InitObject init (get_proc_address, library, initialization);
    init.register_initializer (initialize_engine_lobby);
    init.register_terminator (uninitialize_engine_lobby);
    init.set_minimum_library_initialization_level (MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init ();
}
