plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.match.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

dependencies { testImplementation("org.junit.jupiter:junit-jupiter:5.12.2"); testRuntimeOnly("org.junit.platform:junit-platform-launcher") }
tasks.test { useJUnitPlatform() }
