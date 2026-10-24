plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.zone.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
