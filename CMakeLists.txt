cmake_minimum_required(VERSION 3.20)
project(dsbridge C)

set(CMAKE_C_STANDARD 11)

if(NOT ANDROID)
    message(FATAL_ERROR "This project must be built with the Android NDK toolchain "
                         "(pass -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake)")
endif()

add_executable(dsbridged
    daemon/dsbridged.c
)
target_link_libraries(dsbridged log)

add_library(dsbridge SHARED
    shim/libdsbridge.c
)
set_target_properties(dsbridge PROPERTIES OUTPUT_NAME "dsbridge")
target_link_libraries(dsbridge log)

# Both targets are always defined regardless of ANDROID_ABI; the CI workflow
# only keeps the one relevant to each ABI build (dsbridged for armeabi-v7a,
# libdsbridge.so for arm64-v8a). Building the "wrong" one for a given ABI is
# harmless, it's just discarded.
