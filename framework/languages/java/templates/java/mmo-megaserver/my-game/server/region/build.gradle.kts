plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.region.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
