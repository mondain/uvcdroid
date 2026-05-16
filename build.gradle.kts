plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
    `maven-publish`
}

val groupName = providers.gradleProperty("GROUP").get()
val artifactId = providers.gradleProperty("POM_ARTIFACT_ID").get()
val versionName = providers.gradleProperty("VERSION_NAME").get()

group = groupName
version = versionName

android {
    namespace = "xyz.xrtc.uvcstreamer"
    compileSdk = 35
    ndkVersion = "28.0.12433566"

    defaultConfig {
        minSdk = 21

        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                // NDK r28+ defaults to 16 KB page alignment; keep the flag for older NDKs.
                arguments += "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"
                cppFlags += "-std=c++17"
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
        }
    }
}

dependencies {
    implementation(libs.androidx.annotation)
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = groupName
                this.artifactId = artifactId
                version = versionName

                pom {
                    name.set("uvcdroid")
                    description.set("Android UVC (USB Video Class) streamer library built on libusb + libuvc.")
                    licenses {
                        license {
                            name.set("Apache License 2.0")
                            url.set("https://www.apache.org/licenses/LICENSE-2.0.txt")
                        }
                    }
                }
            }
        }
    }
}
