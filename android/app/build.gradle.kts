plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }

android {
    namespace = "com.wl4.port"
    compileSdk = 34
    ndkVersion = "26.3.11579264"
    defaultConfig {
        applicationId = "com.wl4.port"
        minSdk = 26; targetSdk = 34; versionCode = 1; versionName = "0.1"
        ndk { abiFilters += ((project.findProperty("wl4Abis") as String?) ?: "arm64-v8a,x86_64").split(",") }
        externalNativeBuild { cmake { arguments += "-DWL4_PORT_DIR=${rootDir.parentFile}/port" } }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
    // Fixed signing key: CI runners generate a fresh debug keystore every run, so every APK had a different signature and
    // Android refused to update in place (uninstall -> app data and save lost).  Same key => install over the old app.
    signingConfigs {
        create("fixed") {
            storeFile = file("wl4-debug.jks"); storePassword = "wl4debug"; keyAlias = "wl4"; keyPassword = "wl4debug"
        }
    }
    buildTypes {
        getByName("debug") { signingConfig = signingConfigs.getByName("fixed") }
        release { isMinifyEnabled = false; signingConfig = signingConfigs.getByName("fixed") }
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
dependencies { implementation("androidx.appcompat:appcompat:1.7.0") }
