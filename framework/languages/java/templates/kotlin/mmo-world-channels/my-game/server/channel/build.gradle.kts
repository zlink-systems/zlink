plugins { application }

dependencies { implementation(project(":server:shared")) }

application {
    mainClass.set("MyGame.channel.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
