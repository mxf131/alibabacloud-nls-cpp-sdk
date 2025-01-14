
######uuid使用libevent生成######
set(UUID_C_FLAGS "-fPIC -fvisibility=hidden")
set(UUID_EXTERNAL_COMPILER_FLAGS
    URL ${UUID_URL}
    URL_HASH MD5=${UUID_URL_HASH}
    CONFIGURE_COMMAND ./configure CFLAGS=${UUID_C_FLAGS} enable_shared=no enable_static=yes --prefix=<INSTALL_DIR>
    BUILD_IN_SOURCE 1
    BUILD_COMMAND
      ${ANDROID_NDK}/ndk-build
      -C <INSTALL_DIR>
      NDK_PROJECT_PATH=null
      LOCAL_PATH=<INSTALL_DIR>/src/uuid
      APP_BUILD_SCRIPT=${CMAKE_SOURCE_DIR}/thirdparty/uuid.mk
      NDK_OUT=<INSTALL_DIR>
      NDK_LIBS_OUT=<INSTALL_DIR>
      APP_ABI=${CMAKE_ANDROID_ARCH_ABI}
      NDK_ALL_ABIS=${CMAKE_ANDROID_ARCH_ABI}
      APP_PLATFORM=${ANDROID_PLATFORM}
    )
option(UUID_ENABLE "Enable Uuid." ON)

set(OGG_C_FLAGS "-fPIC -fvisibility=hidden")
set(OGG_EXTERNAL_COMPILER_FLAGS
    URL ${OGG_URL}
    URL_HASH MD5=${OGG_URL_HASH}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DANDROID_PLATFORM_FOLDER=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Debug
      -DCMAKE_SYSTEM_NAME=Android
      -DANDROID_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DANDROID_PLATFORM=${ANDROID_PLATFORM}
      -DANDROID_TARGET_ARCH=${ANDROID_TARGET_ARCH}
      -DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DCMAKE_C_FLAGS=${OGG_C_FLAGS}
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    )
option(OGG_ENABLE "Enable Ogg." ON)

set(OPUS_C_FLAGS "-fPIC -fvisibility=hidden")
set(OPUS_CXX_FLAGS "-fPIC -fvisibility=hidden -ffast-math")
set(OPUS_EXTERNAL_COMPILER_FLAGS
    URL ${OPUS_URL}
    URL_HASH MD5=${OPUS_URL_HASH}
    CONFIGURE_COMMAND ./configure CFLAGS=${OPUS_C_FLAGS} CXXFLAGS=${OPUS_CXX_FLAGS} enable_shared=no enable_static=yes --prefix=<INSTALL_DIR>
    BUILD_IN_SOURCE 1
    BUILD_COMMAND
      ${ANDROID_NDK}/ndk-build
      -C <INSTALL_DIR>
      NDK_PROJECT_PATH=null
      LOCAL_PATH=<INSTALL_DIR>/src/opus
      APP_BUILD_SCRIPT=${CMAKE_SOURCE_DIR}/thirdparty/opus.mk
      NDK_OUT=<INSTALL_DIR>
      NDK_LIBS_OUT=<INSTALL_DIR>
      APP_ABI=${CMAKE_ANDROID_ARCH_ABI}
      NDK_ALL_ABIS=${CMAKE_ANDROID_ARCH_ABI}
      APP_PLATFORM=${ANDROID_PLATFORM}
    )
option(OPUS_ENABLE "Enable Opus." ON)

set(OPENSSL_EXTERNAL_COMPILER_FLAGS
    URL ${OPENSSL_URL}
    URL_HASH MD5=${OPENSSL_URL_HASH}
    CONFIGURE_COMMAND CC=clang LD=${ANDROID_COMPILE_NAME}-ld RANLIB=${ANDROID_COMPILE_NAME}-ranlib AR=${ANDROID_COMPILE_NAME}-ar PATH=${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin:/usr/bin:/bin ./Configure android-${ANDROID_TARGET_ARCH} -D__ANDROID_API__=${ANDROID_API} -fuse-ld="${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/${ANDROID_COMPILE_NAME}-ld" -fPIC threads no-shared no-unit-test no-external-tests -fvisibility=hidden --prefix=<INSTALL_DIR>
    BUILD_IN_SOURCE 1
    BUILD_COMMAND ${MAKE}
    )
option(OPENSSL_ENABLE "Enable Openssl." ON)

set(LIBEVENT_C_FLAGS "-fPIC -fvisibility=hidden")
set(LIBEVENT_EXTERNAL_COMPILER_FLAGS
    URL ${LIBEVENT_URL}
    URL_HASH MD5=${LIBEVENT_URL_HASH}
    CONFIGURE_COMMAND ./configure CFLAGS=${LIBEVENT_C_FLAGS} enable_debug_mode=yes enable_static=yes enable_shared=no --disable-openssl --prefix=<INSTALL_DIR>
    BUILD_IN_SOURCE 1
    BUILD_COMMAND
      ${ANDROID_NDK}/ndk-build
      -C <INSTALL_DIR>
      NDK_PROJECT_PATH=null
      LOCAL_PATH=<INSTALL_DIR>/src/libevent
      OTHER_PATH=${CMAKE_SOURCE_DIR}/thirdparty/libevent_android
      APP_BUILD_SCRIPT=${CMAKE_SOURCE_DIR}/thirdparty/libevent.mk
      NDK_OUT=<INSTALL_DIR>
      NDK_LIBS_OUT=<INSTALL_DIR>
      APP_ABI=${CMAKE_ANDROID_ARCH_ABI}
      NDK_ALL_ABIS=${CMAKE_ANDROID_ARCH_ABI}
      APP_PLATFORM=${ANDROID_PLATFORM}
    )
option(LIBEVENT_ENABLE "Enable Libevent." ON)

set(JSONCPP_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -fvisibility=hidden -D_GLIBCXX_USE_CXX11_ABI=0 -g -ggdb")
set(JSONCPP_EXTERNAL_COMPILER_FLAGS
    URL ${JSONCPP_URL}
    URL_HASH MD5=${JSONCPP_URL_HASH}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DANDROID_PLATFORM_FOLDER=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Debug
      -DCMAKE_SYSTEM_NAME=Android
      -DANDROID_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DANDROID_PLATFORM=${ANDROID_PLATFORM}
      -DANDROID_TARGET_ARCH=${ANDROID_TARGET_ARCH}
      -DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DCMAKE_CXX_FLAGS=${JSONCPP_CXX_FLAGS}
      -DJSONCPP_WITH_TESTS=OFF
      -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF
      -DJSONCPP_WITH_WARNING_AS_ERROR=OFF
      -DJSONCPP_WITH_PKGCONFIG_SUPPORT=OFF
      -DJSONCPP_WITH_CMAKE_PACKAGE=OFF
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    )
option(JSONCPP_ENABLE "Enable Jsoncpp." ON)

set(CURL_C_FLAGS "-fPIC -fvisibility=hidden -D_GLIBCXX_USE_CXX11_ABI=0")
set(CURL_CXX_FLAGS "-fPIC -fvisibility=hidden -D_GLIBCXX_USE_CXX11_ABI=0")
set(CURL_EXTERNAL_COMPILER_FLAGS
    URL ${CURL_URL}
    URL_HASH MD5=${CURL_URL_HASH}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DANDROID_PLATFORM_FOLDER=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_SYSTEM_NAME=Android
      -DANDROID_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DANDROID_PLATFORM=${ANDROID_PLATFORM}
      -DANDROID_TARGET_ARCH=${ANDROID_TARGET_ARCH}
      -DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}
      -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DCMAKE_C_FLAGS=${CURL_C_FLAGS}
      -DCMAKE_CXX_FLAGS=${CURL_CXX_FLAGS}
      -DCMAKE_USE_OPENSSL=ON
      -DCMAKE_FIND_ROOT_PATH=<INSTALL_DIR>/../openssl-prefix
      -DOPENSSL_ROOT_DIR=<INSTALL_DIR>/../openssl-prefix
      -DOPENSSL_LIBRARIES=<INSTALL_DIR>/../openssl-prefix/lib
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    )
option(CURL_ENABLE "Enable Curl." ON)

