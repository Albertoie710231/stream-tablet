# StreamTablet ProGuard Rules

# Keep model classes
-keep class com.streamtablet.network.** { *; }
-keep class com.streamtablet.video.** { *; }
-keep class com.streamtablet.input.** { *; }

# Keep data classes
-keepclassmembers class * {
    @kotlin.Metadata *;
}
