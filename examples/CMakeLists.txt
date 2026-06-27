# One example per target. Each needs a GL header and links the platform library.
if(EMSCRIPTEN)
    add_executable(hello_web hello_web.c)
    target_link_libraries(hello_web PRIVATE platform)
    set_target_properties(hello_web PROPERTIES
        SUFFIX ".html"
        LINK_FLAGS "-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1")
elseif(ANDROID)
    # The loadable NativeActivity module: provides main(), pulls in the static
    # platform lib (which carries android_main and the glue entry point).
    add_library(hello_android SHARED hello_android.c)
    target_link_libraries(hello_android PRIVATE platform)
else()
    add_executable(hello_x11 hello_x11.c)
    target_link_libraries(hello_x11 PRIVATE platform)
    find_package(OpenGL REQUIRED)
    target_link_libraries(hello_x11 PRIVATE OpenGL::GL)

    add_executable(raytrace  raytrace.c)
    target_link_libraries(raytrace PRIVATE platform m)

    add_executable(uitest  uitest.c)
    target_link_libraries(uitest PRIVATE platform m)
    
        add_executable(mwidgets  mwidgets.c)
    target_link_libraries(mwidgets PRIVATE platform m)

        add_executable(ddemo  ddemo.c)
    target_link_libraries(ddemo PRIVATE platform m)

        add_executable(galaxy  galaxy.c)
    target_link_libraries(galaxy PRIVATE platform m)
endif()
