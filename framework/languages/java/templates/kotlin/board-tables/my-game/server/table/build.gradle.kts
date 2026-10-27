plugins { application }

dependencies { implementation(project(":server:shared")) }

application {
    mainClass.set("MyGame.table.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
