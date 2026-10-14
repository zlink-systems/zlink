plugins { application }
dependencies { implementation(project(":server:shared")) }
application {
    mainClass.set("MyGame.instance.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
