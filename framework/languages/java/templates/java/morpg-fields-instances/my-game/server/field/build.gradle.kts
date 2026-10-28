plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.field.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
