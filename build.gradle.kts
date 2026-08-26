plugins {
    id("com.android.library") version "9.3.2" apply false
}

group = "io.github.thor2002ro"
version = providers.gradleProperty("VERSION_NAME").get()

tasks.register("publishLocalArtifacts") {
    group = "publishing"
    description = "Publishes the release AAR and copies matching native SDK artifacts."
    dependsOn(
        ":lib:publishReleasePublicationToOutputRepository",
        ":lib:copySdkArtifacts",
    )
}
