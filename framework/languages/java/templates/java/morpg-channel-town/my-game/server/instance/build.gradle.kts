plugins { application }
dependencies { implementation(project(":server:shared"))  }
application {
    mainClass.set("MyGame.instance.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

dependencies { testImplementation("org.junit.jupiter:junit-jupiter:5.11.4"); testRuntimeOnly("org.junit.platform:junit-platform-launcher:1.11.4") }
tasks.test { useJUnitPlatform() }
