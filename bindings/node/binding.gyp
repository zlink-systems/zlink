{
  "targets": [
    {
      "target_name": "zlink",
      "sources": [
        "native/src/addon.cc",
        "native/src/addon_core.cc",
        "native/src/addon_core_options.cc",
        "native/src/addon_exports.cc",
        "native/src/addon_request_callbacks.cc"
      ],
      "include_dirs": [ "<!(node scripts/resolve_core.js include)" ],
      "variables": {
        "zlink_lib%": "<!(node scripts/resolve_core.js library)"
      },
      "conditions": [
        [ "\"<(zlink_lib)\" != \"\"", { "libraries": [ "<(zlink_lib)" ] } ],
        [ "OS==\"linux\"", { "ldflags": [ "-Wl,-rpath,\\$$ORIGIN" ] } ],
        [ "OS==\"mac\"", {
          "ldflags": [ "-Wl,-rpath,@loader_path" ],
          "xcode_settings": {
            "LD_RUNPATH_SEARCH_PATHS": [ "@loader_path" ]
          }
        } ]
      ]
    }
  ]
}
