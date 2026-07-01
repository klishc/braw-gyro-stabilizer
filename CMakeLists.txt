# Copyright (c) 2026 Niko Klishchenko. All rights reserved.
cmake_minimum_required(VERSION 3.21)
project(BRAWGyroStabilizer VERSION 1.0.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")

# ── SDK paths ─────────────────────────────────────────────────────────────────
# All SDKs are expected under one root (default ~/SDKs). Override the root with
# -DSDK_ROOT=<path> or the SDK_ROOT environment variable, or point each SDK
# individually with -DPR_SDK / -DBRAW_SDK / -DAE_SDK.
if(NOT SDK_ROOT)
    if(DEFINED ENV{SDK_ROOT})
        set(SDK_ROOT "$ENV{SDK_ROOT}")
    else()
        set(SDK_ROOT "$ENV{HOME}/SDKs")
    endif()
endif()

if(NOT PR_SDK)
    set(PR_SDK "${SDK_ROOT}/Premiere Pro 26.0 C++ SDK/Examples/Headers")
endif()
if(NOT BRAW_SDK)
    set(BRAW_SDK "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac")
endif()

# After Effects SDK — REQUIRED. The AE-compatible effect entry point pulls in
# AE_Effect.h / AE_EffectCB.h via PrSDKAESupport.h; those ship ONLY in the free
# After Effects SDK, not the Premiere SDK. Point AE_SDK at the SDK root (the
# folder that contains Examples/). Override with -DAE_SDK=<path> or the
# AE_SDK_BASE_PATH environment variable (matching Adobe's own sample projects).
if(NOT AE_SDK)
    if(DEFINED ENV{AE_SDK_BASE_PATH})
        set(AE_SDK "$ENV{AE_SDK_BASE_PATH}")
    else()
        set(AE_SDK "${SDK_ROOT}/After Effects SDK")
    endif()
endif()
if(NOT EXISTS "${AE_SDK}/Examples/Headers/AE_Effect.h")
    message(FATAL_ERROR
        "After Effects SDK not found.\n"
        "  Looked for: ${AE_SDK}/Examples/Headers/AE_Effect.h\n"
        "  Download the free After Effects SDK from the Adobe developer site,\n"
        "  unzip it, and either place it at ${SDK_ROOT}/After Effects SDK\n"
        "  or pass -DAE_SDK=<sdk-root> / set AE_SDK_BASE_PATH.")
endif()

if(NOT PREBUILT_RSRC)
    message(FATAL_ERROR "PREBUILT_RSRC not set — run via build.sh")
endif()

# ── Apple frameworks ──────────────────────────────────────────────────────────
find_library(METAL_FW          Metal)
find_library(FOUNDATION_FW     Foundation)
find_library(COREFOUNDATION_FW CoreFoundation)

# ── Plugin bundle ─────────────────────────────────────────────────────────────
add_library(BRAWGyroStabilizer MODULE
    src/plugin/PluginMain.cpp
    src/plugin/PluginMain.h
    src/braw/BRAWReader.cpp
    src/braw/BRAWReader.h
    src/gyro/GyroIntegrator.cpp
    src/gyro/GyroIntegrator.h
    src/gyro/WarpMap.h
    src/warp/MetalWarpEngine.mm
    src/warp/MetalWarpEngine.h
    src/plugin/GPUFilter.mm              # GPU (SmartFX) render path
)

# Premiere's GPU video-filter framework header lives in the SDK's sample Utils dir.
set(PR_GPU_UTILS "${SDK_ROOT}/Premiere Pro 26.0 C++ SDK/Examples/Projects/GPUVideoFilter/Utils")

target_include_directories(BRAWGyroStabilizer PRIVATE
    "${PR_SDK}"                          # Premiere headers first — its AEConfig.h/SPTypes.h win
    "${AE_SDK}/Examples/Headers"         # real AE_Effect.h / AE_EffectCB.h / AE_Macros.h …
    "${AE_SDK}/Examples/Headers/SP"      # SPBasic.h and friends
    "${AE_SDK}/Examples/Util"
    "${PR_GPU_UTILS}"                    # PrGPUFilterModule.h
    "${BRAW_SDK}/Include"
    src/
)

# CoreFoundation headers for CFStringRef used in BRAWReader
find_library(COREFOUNDATION_FW CoreFoundation REQUIRED)
target_link_libraries(BRAWGyroStabilizer PRIVATE ${COREFOUNDATION_FW})

# BlackmagicRawAPI is a framework — it needs a FRAMEWORK search path (-F), not a
# library path (-L). target_link_directories only emits -L, so point the linker at
# the framework's directory explicitly.
target_link_options(BRAWGyroStabilizer PRIVATE "-F${BRAW_SDK}/Libraries")

# The binary links BlackmagicRawAPI as @rpath/...; add an rpath so the loader finds
# the framework we embed at Contents/Frameworks (binary lives at Contents/MacOS).
set_target_properties(BRAWGyroStabilizer PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@loader_path/../Frameworks")

target_link_libraries(BRAWGyroStabilizer PRIVATE
    "-framework BlackmagicRawAPI"
    "-framework Metal"
    "-framework Foundation"
    "-framework CoreFoundation"
)

# ── Bundle ────────────────────────────────────────────────────────────────────
set_target_properties(BRAWGyroStabilizer PROPERTIES
    BUNDLE               TRUE
    BUNDLE_EXTENSION     "plugin"   # Premiere's effect loader recognizes .plugin, not .bundle
    OUTPUT_NAME          "GyroStabV4"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
)

# PkgInfo content (type "eFKT" + creator "FXTC"), written at configure time so the
# POST_BUILD step is a plain copy with no shell quoting/ordering pitfalls.
file(WRITE "${CMAKE_BINARY_DIR}/PkgInfo" "eFKTFXTC")

# ── Copy PiPL rsrc + PkgInfo + BRAW framework into bundle ─────────────────────
add_custom_command(TARGET BRAWGyroStabilizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Resources"
    # The PiPL resource file must be named after the bundle's executable
    # (e.g. GyroStabV4.rsrc) — Premiere's effect loader looks it up by that name.
    COMMAND ${CMAKE_COMMAND} -E copy "${PREBUILT_RSRC}"
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Resources/$<TARGET_FILE_BASE_NAME:BRAWGyroStabilizer>.rsrc"
    # App/bundle icon (CFBundleIconFile=logo → Resources/logo.icns), shown in Finder.
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/assets/logo.icns"
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Resources/logo.icns"
    # PkgInfo: AE/Premiere effects carry an 8-byte type+creator ("eFKT" + "FXTC").
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/PkgInfo"
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/PkgInfo"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Frameworks"
    # Use cp -R (not cmake -E copy_directory): the BRAW framework relies on
    # Versions/Current symlinks, and copy_directory dereferences them, producing a
    # malformed framework that codesign rejects ("bundle format is ambiguous").
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Frameworks/BlackmagicRawAPI.framework"
    COMMAND cp -R
        "${BRAW_SDK}/Libraries/BlackmagicRawAPI.framework"
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Frameworks/"
    COMMENT "Copying resources and BRAW framework into bundle"
)

# ── Ad-hoc sign (inside-out: framework first, then the bundle; no --deep) ──────
# Strip extended attributes first (xattrs carried in by cp -R / Rez output trigger
# codesign's "resource fork, Finder information, or similar detritus" error).
add_custom_command(TARGET BRAWGyroStabilizer POST_BUILD
    COMMAND xattr -cr "$<TARGET_BUNDLE_DIR:BRAWGyroStabilizer>"
    COMMAND codesign --force --sign -
        "$<TARGET_BUNDLE_CONTENT_DIR:BRAWGyroStabilizer>/Frameworks/BlackmagicRawAPI.framework"
    COMMAND codesign --force --sign -
        "$<TARGET_BUNDLE_DIR:BRAWGyroStabilizer>"
    COMMENT "Ad-hoc signing"
)

# ── Install ───────────────────────────────────────────────────────────────────
set(PR_PLUGINS "$ENV{HOME}/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore")
install(TARGETS BRAWGyroStabilizer DESTINATION "${PR_PLUGINS}")
