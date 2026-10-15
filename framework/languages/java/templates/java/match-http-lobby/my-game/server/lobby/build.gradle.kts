plugins { application }
dependencies {
    implementation(project(":server:shared"))
    implementation(platform("org.springframework.boot:spring-boot-dependencies:3.5.14"))
    implementation("io.netty:netty-codec-http")
}
application {
    mainClass.set("MyGame.lobby.Program")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

dependencies {
    testImplementation("org.junit.jupiter:junit-jupiter:${rootProject.extra["junitVersion"]}")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}
tasks.test { useJUnitPlatform() }
