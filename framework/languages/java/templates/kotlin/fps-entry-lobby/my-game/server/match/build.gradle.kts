plugins { application }

dependencies {
    implementation(project(":server:shared"))
    testImplementation(kotlin("test-junit"))
}

application {
    mainClass.set("MyGame.match.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
