plugins { application }

dependencies { implementation(project(":server:shared")) }

application {
    mainClass.set("MyGame.accounts.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
