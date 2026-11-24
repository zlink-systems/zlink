plugins { application }

dependencies {
    implementation("systems.zlink:zlink-stream-connector:0.34.0")
    implementation("org.jspecify:jspecify:1.0.0")
}

java { toolchain { languageVersion = JavaLanguageVersion.of(25) } }

application {
    mainClass = "examples.ConnectorExample"
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
