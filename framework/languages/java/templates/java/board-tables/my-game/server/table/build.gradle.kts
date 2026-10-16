plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.table.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
