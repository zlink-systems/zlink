plugins { application }

dependencies {
    implementation(project(":server:shared"))
    implementation("io.lettuce:lettuce-core:7.6.0.RELEASE")
}

application {
    mainClass.set("MyGame.api.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
