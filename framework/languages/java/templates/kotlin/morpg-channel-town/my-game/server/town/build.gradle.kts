plugins { application }
dependencies { implementation(project(":server:shared")) }
application {
    mainClass.set("MyGame.town.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
