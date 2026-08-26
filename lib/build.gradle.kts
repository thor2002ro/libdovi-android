import org.gradle.api.publish.maven.MavenPublication

plugins {
    id("com.android.library")
    `maven-publish`
}

group = "io.github.thor2002ro"
version = providers.gradleProperty("VERSION_NAME").get()

val supportedAbis = listOf("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
val nativeOutput = rootProject.layout.projectDirectory.dir("OUTPUT/native")
val sdkOutput = rootProject.layout.projectDirectory.dir("OUTPUT/sdk")

android {
    namespace = "io.github.thor2002ro.libdovi"
    compileSdk = 36

    defaultConfig {
        minSdk = 24
    }

    sourceSets {
        getByName("main").jniLibs.directories.add(nativeOutput.asFile.absolutePath)
    }

    packaging {
        jniLibs.keepDebugSymbols += "**/libjellyfin_dovi.so"
    }

    publishing {
        singleVariant("release")
    }
}

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = project.group.toString()
            artifactId = "libdovi-android"
            version = project.version.toString()

            afterEvaluate {
                from(components["release"])
            }
        }
    }
    repositories {
        maven {
            name = "output"
            url = rootProject.layout.projectDirectory.dir("OUTPUT/maven").asFile.toURI()
        }
    }
}

tasks.register("copySdkArtifacts") {
    group = "publishing"
    description = "Copies the exact AAR JNI libraries and native development files into OUTPUT/sdk."
    inputs.dir(nativeOutput)
    outputs.dir(sdkOutput)

    doLast {
        supportedAbis.forEach { abi ->
            val source = nativeOutput.dir(abi).asFile
            val library = source.resolve("libjellyfin_dovi.so")
            val header = source.resolve("include/dovi.h")
            val pkgConfig = source.resolve("lib/pkgconfig/jellyfin-dovi.pc")

            listOf(library, header, pkgConfig).forEach { file ->
                check(file.isFile) { "Missing native SDK artifact: ${file.absolutePath}" }
            }

            copy {
                from(library)
                into(sdkOutput.dir("$abi/lib").asFile)
            }
            copy {
                from(header)
                into(sdkOutput.dir("$abi/include").asFile)
            }
            copy {
                from(pkgConfig)
                into(sdkOutput.dir("$abi/lib/pkgconfig").asFile)
            }
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
