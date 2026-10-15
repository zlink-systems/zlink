plugins { application }

dependencies { implementation(project(":server:shared")) }

application {
    mainClass.set("MyGame.field.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
