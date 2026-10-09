cmake_minimum_required(VERSION 3.20)
project(ScopeDeck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# GLFW
# ---------------------------------------------------------------------------
# Only the library - the docs/tests/examples targets are not built, and their
# directories are stripped from the vendored copy anyway (see vendor/VENDOR.md).
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/glfw)

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
# Built as a static lib from source rather than compiled into the app target, so
# ImGui's own warnings do not have to be read past on every build of our code.
add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/backends/imgui_impl_glfw.cpp
    vendor/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    vendor/imgui
    vendor/imgui/backends
)

target_link_libraries(imgui PUBLIC glfw)

# ---------------------------------------------------------------------------
# stb_image - decodes a user-uploaded still (Compare panel)
# ---------------------------------------------------------------------------
# Its own static lib for the same reason ImGui above is: /W4 has no business
# being read past on vendored code, and this way it never has to be.
add_library(stb_image STATIC app/StbImageImpl.cpp)
target_include_directories(stb_image PUBLIC vendor/stb)

# ---------------------------------------------------------------------------
# scopecore - the wire format and its reader
# ---------------------------------------------------------------------------
# No ImGui, no GLFW, no GL. Kept that way on purpose: the format and the seqlock
# are the part that has to be testable without a window, the same way the old
# tree's core/ included no OFX header.
add_library(scopecore STATIC
    core/ScopeReader.cpp
)

target_include_directories(scopecore PUBLIC core)

# ---------------------------------------------------------------------------
# The app
# ---------------------------------------------------------------------------
add_executable(scopedeck
    app/main.cpp
    app/AudioMeterBridge.cpp
    app/AudioAnalysis.cpp
    app/PanelCommon.cpp
    app/ScopeImages.cpp
    app/Texture.cpp
    app/Theme.cpp
    app/TimecodeBridge.cpp
    app/SubtitleBridge.cpp
    app/StillImage.cpp
    app/ScreenCapture.cpp
    # Screen Capture runs the tap's own reduction in-process, so the app gets
    # the engine - but only the engine: ScopeShm.cpp would redefine kShmName,
    # which ScopeReader.cpp already defines, and the app needs no publisher.
    core/ScopeCore.cpp
    # Preview Scale / Row Step handed to the Premiere plugin (Input menu).
    core/ScopeControl.cpp
)

target_include_directories(scopedeck PRIVATE app)
target_link_libraries(scopedeck PRIVATE scopecore imgui stb_image)

if(WIN32)
    # Phase 1 calls only GL 1.1 entry points, which opengl32 exports directly -
    # see the note at the top of app/Texture.h about why there is no loader yet.
    target_link_libraries(scopedeck PRIVATE opengl32)

    # OpenStillFileDialog (StillImage.cpp) is a plain GetOpenFileNameW call,
    # which lives in comdlg32, not one of the libraries already linked above.
    target_link_libraries(scopedeck PRIVATE comdlg32)

    # ActivateAudioInterfaceAsync (AudioMeterBridge.cpp's per-process loopback
    # capture) is exported by Mmdevapi.dll - CoCreateInstance-based WASAPI
    # calls resolve their DLL through COM and need no explicit import lib,
    # but this one is a plain DLL export, called directly by name.
    target_link_libraries(scopedeck PRIVATE mmdevapi)

    # timeBeginPeriod (ScreenCapture.cpp): without it the capture thread's
    # sleep rounds to ~15.6 ms and a 60 Hz capture runs at ~30.
    target_link_libraries(scopedeck PRIVATE winmm)

    # DwmSetWindowAttribute (main.cpp): Solo Fills the Screen turns Windows'
    # own minimize/maximize animations off for the moment it switches.
    target_link_libraries(scopedeck PRIVATE dwmapi)

    # A GUI app, so launching it does not also open a console window. Keeps main()
    # rather than WinMain() as the entry point.
    set_target_properties(scopedeck PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(scopedeck PRIVATE "/ENTRY:mainCRTStartup")

    # App icon - see the comment in the .rc for why its resource is named
    # GLFW_ICON. The .ico is committed; tools/make_icon.py rebuilds it.
    target_sources(scopedeck PRIVATE app/icon/scopedeck.rc)

    # TimecodeBridge shells out to this script and resolves it next to the .exe
    # (see TimecodeBridge.cpp) rather than trusting the process's inherited
    # working directory, which is wrong whenever the exe isn't launched from
    # this source tree's root (e.g. double-clicked from build/).
    add_custom_command(TARGET scopedeck POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/timecode_poll_worker.py"
            "$<TARGET_FILE_DIR:scopedeck>/timecode_poll_worker.py"
    )

    # SubtitleBridge shells out to this script the same way TimecodeBridge
    # does to its own - see that copy step's comment just above.
    add_custom_command(TARGET scopedeck POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/subtitle_poll_worker.py"
            "$<TARGET_FILE_DIR:scopedeck>/subtitle_poll_worker.py"
    )
elseif(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(scopedeck PRIVATE ${OPENGL_FRAMEWORK})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(scopedeck PRIVATE OpenGL::GL)
endif()

if(MSVC)
    target_compile_options(scopedeck PRIVATE /W4 /permissive-)
    target_compile_options(scopecore PRIVATE /W4 /permissive-)
    # Silence the CRT's own deprecation of snprintf and friends. The alternatives
    # are MSVC-only, and this code has to build on Apple clang too.
    target_compile_definitions(scopedeck PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(scopedeck PRIVATE -Wall -Wextra)
    target_compile_options(scopecore PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# scopedeck_probe - headless reader check
# ---------------------------------------------------------------------------
# Console subsystem, no window, no GL: answers "is the tap publishing, and at
# which wire version" without going through the UI that is usually the thing
# under suspicion.
add_executable(scopedeck_probe tools/ScopeProbe.cpp app/ScopeImages.cpp)
target_link_libraries(scopedeck_probe PRIVATE scopecore)
target_include_directories(scopedeck_probe PRIVATE app)

if(MSVC)
    target_compile_options(scopedeck_probe PRIVATE /W4 /permissive-)
    target_compile_definitions(scopedeck_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(scopedeck_probe PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# scopecore_writer, scope_selftest, ScopeTap - the OFX plugin
# ---------------------------------------------------------------------------
# The write side of the wire format scopecore (above) reads - migrated in
# from "Scope Deck (old)"/{core,plugin,tools}, so this tree no longer
# depends on that one for anything. A separate library from scopecore
# rather than merged into it: this app never needs OFX or the reduction
# engine, and the plugin never needs ScopeReader - two different consumers
# of the one core/ScopeTypes.h, not one thing artificially split in two.
#
# Resolve's OpenFX SDK is vendored under vendor/openfx; no external download.

find_package(Threads REQUIRED)

add_library(scopecore_writer STATIC
    core/ScopeCore.cpp
    core/ScopeCore.h
    core/ScopeShm.cpp
    core/ScopeShm.h
)

target_include_directories(scopecore_writer PUBLIC core)
target_link_libraries(scopecore_writer PUBLIC Threads::Threads)
set_property(TARGET scopecore_writer PROPERTY POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_compile_definitions(scopecore_writer PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set_target_properties(scopecore_writer PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
else()
    target_link_libraries(scopecore_writer PUBLIC rt)   # shm_open
endif()

# Drives scopecore_writer with synthetic frames, so the reduction engine and
# wire format can be verified without Resolve running.
add_executable(scope_selftest tools/ScopeSelfTest.cpp)
target_link_libraries(scope_selftest PRIVATE scopecore_writer)

if(WIN32)
    target_compile_definitions(scope_selftest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# ScopeTransmit - the Premiere Pro counterpart to ScopeTap
# ---------------------------------------------------------------------------
# A Transmit plugin (Preferences > Playback > Video Device) publishing into
# the same shared memory as ScopeTap - see plugin/ScopeTransmit.cpp.
#
# Needs Adobe's Premiere Pro C++ SDK, which is NOT in this repo: its headers
# are marked Adobe Confidential, so each machine downloads its own from the
# Adobe Developer Console. Without it this target is skipped, exactly as
# ScopeTap is without an OpenFX SDK.
if(WIN32)
    set(PREMIERE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/adobe premiere sdk/Windows/Premiere Pro 26.0 C++ SDK"
        CACHE PATH "Root of Adobe's Premiere Pro C++ SDK (the folder holding Examples/Headers)")

    if(NOT EXISTS "${PREMIERE_SDK_DIR}/Examples/Headers/PrSDKTransmit.h")
        message(STATUS "Premiere Pro SDK not found at ${PREMIERE_SDK_DIR} - skipping ScopeTransmit.")
    else()
        add_library(ScopeTransmit MODULE plugin/ScopeTransmit.cpp core/ScopeControl.cpp core/ScopeControl.h)
        target_include_directories(ScopeTransmit PRIVATE "${PREMIERE_SDK_DIR}/Examples/Headers")
        target_link_libraries(ScopeTransmit PRIVATE scopecore_writer)
        # No NOMINMAX here: PrSDKTypes.h defines it itself.
        target_compile_definitions(ScopeTransmit PRIVATE PRWIN_ENV _CRT_SECURE_NO_WARNINGS)
        # Premiere loads MediaCore plugins by extension: .prm on Windows.
        set_target_properties(ScopeTransmit PROPERTIES PREFIX "" SUFFIX ".prm" OUTPUT_NAME "ScopeTransmit")
    endif()
endif()

# ---------------------------------------------------------------------------
# scope_conformance - the acceptance gate for every reduction backend.
#
# Diffs the real ScopeEngine against a plain reference implementation over a
# deterministic frame set, bit-exactly. The CUDA path joins as one more
# candidate in the same loop when it lands.
#
# Float contraction must stay off here. Bin assignment runs a luma dot product
# through a scale-and-floor, so a fused multiply-add moves boundary values into
# neighbouring bins and every exact comparison stops meaning anything. MSVC does
# not contract under the default /fp:precise, and does not emit FMA at all
# without /arch:AVX2, but stating it is cheaper than rediscovering it; the
# harness also checks at runtime and refuses to run if it is wrong. The CUDA
# target, when it exists, needs -fmad=false for the same reason.
# ---------------------------------------------------------------------------
add_executable(scope_conformance tools/ScopeConformance.cpp)
target_link_libraries(scope_conformance PRIVATE scopecore_writer)

if(MSVC)
    target_compile_options(scope_conformance PRIVATE /W4 /permissive- /fp:precise)
    target_compile_definitions(scope_conformance PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(scope_conformance PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

# ---------------------------------------------------------------------------
# scope_publish_contention - several publishers on one scratch block against
# the app's real ScopeReader, checking no frame it accepts is a torn mix of two
# writes, the write counter never goes backwards and no publish index is
# reused. Links the reader and the writer together, which is why kShmName is
# defined once in ScopeTypes.h. No GPU needed; release.ps1 runs it.
# ---------------------------------------------------------------------------
add_executable(scope_publish_contention tools/ScopePublishContention.cpp)
target_link_libraries(scope_publish_contention PRIVATE scopecore scopecore_writer)

if(MSVC)
    target_compile_options(scope_publish_contention PRIVATE /W4 /permissive-)
    target_compile_definitions(scope_publish_contention PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(scope_publish_contention PRIVATE -Wall -Wextra)
endif()

# Defaults to the copy vendored in this tree (vendor/openfx, see VENDOR.md),
# so a machine with no Resolve install - a CI runner - builds the same plugin.
# Pass -DOFX_SDK_DIR to build against a Resolve install's own copy instead.
set(OFX_SDK_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/openfx")

set(OFX_SDK_DIR "${OFX_SDK_DEFAULT}" CACHE PATH
    "Root of Resolve's OpenFX SDK (the folder holding OpenFX-1.4 and Support)")

if(NOT EXISTS "${OFX_SDK_DIR}/Support/include/ofxsImageEffect.h")
    message(WARNING
        "OpenFX SDK not found at ${OFX_SDK_DIR} - skipping the ScopeTap target. "
        "scopecore_writer and scope_selftest will still build. "
        "Pass -DOFX_SDK_DIR=<path> to enable the plugin.")
else()
    # The OFX C++ wrapper, built from source alongside the plugin. ofxsHWNDInteract
    # is deliberately excluded: Windows-only with no macOS counterpart, and the
    # SDK's own Visual Studio project leaves it out too.
    set(OFX_SUPPORT_SOURCES
        "${OFX_SDK_DIR}/Support/Library/ofxsCore.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsImageEffect.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsInteract.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsLog.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsMultiThread.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsParams.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsProperty.cpp"
        "${OFX_SDK_DIR}/Support/Library/ofxsPropertyValidation.cpp"
    )

    add_library(ScopeTap MODULE
        plugin/ScopeTap.cpp
        plugin/ScopeTap.h
        ${OFX_SUPPORT_SOURCES}
    )

    target_include_directories(ScopeTap PRIVATE
        "${OFX_SDK_DIR}/OpenFX-1.4/include"
        "${OFX_SDK_DIR}/Support/include"
    )

    target_link_libraries(ScopeTap PRIVATE scopecore_writer)

    # An OFX plugin is a shared library with a .ofx extension and no lib prefix.
    set_target_properties(ScopeTap PROPERTIES
        PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "ScopeTap")

    if(WIN32)
        # The SDK headers gate on WIN32/WIN64, which MSVC does not define itself
        # (it defines _WIN32/_WIN64).
        target_compile_definitions(ScopeTap PRIVATE
            WIN32 WIN64 NOMINMAX _CRT_SECURE_NO_WARNINGS)
        set(OFX_BUNDLE_ARCH_DIR "Win64")
    elseif(APPLE)
        set_target_properties(ScopeTap PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
        set(OFX_BUNDLE_ARCH_DIR "MacOS")
    else()
        set(OFX_BUNDLE_ARCH_DIR "Linux-x86-64")
    endif()

    # Bundle layout: ScopeTap.ofx.bundle/Contents/<arch>/ScopeTap.ofx
    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/bundle/ScopeTap.ofx.bundle")

    add_custom_command(TARGET ScopeTap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${OFX_BUNDLE_DIR}/Contents/${OFX_BUNDLE_ARCH_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${OFX_BUNDLE_DIR}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:ScopeTap>"
                "${OFX_BUNDLE_DIR}/Contents/${OFX_BUNDLE_ARCH_DIR}/ScopeTap.ofx"
        COMMENT "Packaging ScopeTap.ofx.bundle"
        VERBATIM)

    # ------------------------------------------------------------------
    # ScopeTapGpuSpike - the throwaway CUDA measurement plugin.
    #
    # Its own target and its own bundle, never linked into ScopeTap: the spike
    # exists to find out whether vendor GPU code can safely live inside
    # Resolve's process, and that question must not be answered at the working
    # tap's expense. Skipped entirely, with a note, when no CUDA compiler is
    # present - the normal build must not start depending on a toolkit.
    # ------------------------------------------------------------------
    include(CheckLanguage)
    check_language(CUDA)

    if(NOT CMAKE_CUDA_COMPILER)
        message(STATUS
            "No CUDA compiler found - skipping ScopeTapGpuSpike. "
            "Install CUDA Toolkit 12.8 or 12.9 to build it (NOT 13.x: 13.0 "
            "dropped offline compilation for sm_50-sm_70, which the "
            "architecture list below still covers).")
    else()
        enable_language(CUDA)

        # SASS for the architectures that actually run Resolve, plus two PTX
        # floors. PTX only ever JITs *forward*, so compute_52 is what keeps an
        # unanticipated architecture below sm_120 loading at all rather than
        # failing outright; compute_120 gives newer cards better PTX than that.
        # sm_120 is RTX 50-series consumer Blackwell (compute capability 12.0) -
        # note it is NOT covered by sm_100, which is datacenter Blackwell.
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.8)
            message(WARNING
                "CUDA ${CMAKE_CUDA_COMPILER_VERSION} cannot emit sm_120 SASS; "
                "RTX 50-series cards will fall back to JIT from PTX. "
                "Use 12.8 or 12.9 for native support.")
            set(SPIKE_CUDA_ARCHS 52-real 61-real 75-real 86-real 89-real 52-virtual)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            message(WARNING
                "CUDA ${CMAKE_CUDA_COMPILER_VERSION} dropped Maxwell/Pascal/Volta "
                "offline compilation - building without the sm_52/sm_61 floor. "
                "Use 12.8 or 12.9 if those cards matter.")
            set(SPIKE_CUDA_ARCHS 75-real 86-real 89-real 120-real 75-virtual 120-virtual)
        else()
            set(SPIKE_CUDA_ARCHS 52-real 61-real 75-real 86-real 89-real 120-real
                                 52-virtual 120-virtual)
        endif()

        add_library(ScopeTapGpuSpike MODULE
            plugin/ScopeTapGpuSpike.cpp
            plugin/ScopeTapGpuSpike.h
            plugin/spike/ScopeGpuSpike.cu
            plugin/spike/ScopeGpuSpike.h
            ${OFX_SUPPORT_SOURCES}
        )

        target_include_directories(ScopeTapGpuSpike PRIVATE
            "${OFX_SDK_DIR}/OpenFX-1.4/include"
            "${OFX_SDK_DIR}/Support/include"
            plugin
        )

        set_target_properties(ScopeTapGpuSpike PROPERTIES
            PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "ScopeTapGpuSpike"
            CUDA_ARCHITECTURES "${SPIKE_CUDA_ARCHS}"
            CUDA_SEPARABLE_COMPILATION OFF
            # Static cudart so an end user needs only an NVIDIA driver, never a
            # CUDA toolkit install, and no runtime DLL has to sit beside the .ofx.
            CUDA_RUNTIME_LIBRARY Static)

        # sm_52/sm_61 are already deprecated in CUDA 12.9 (removed in 13.0). The
        # architecture list above keeps them on purpose; silence the per-file
        # notice so it does not bury a real warning.
        target_compile_options(ScopeTapGpuSpike PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>)

        if(WIN32)
            target_compile_definitions(ScopeTapGpuSpike PRIVATE
                WIN32 WIN64 NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()

        set(SPIKE_BUNDLE_DIR "${CMAKE_BINARY_DIR}/bundle/ScopeTapGpuSpike.ofx.bundle")

        # ------------------------------------------------------------------
        # The real tap gains the CUDA reduction here, not at its own target
        # definition above, because enable_language(CUDA) has only just
        # happened. A build directory configured without a CUDA compiler still
        # produces a perfectly good CPU-only ScopeTap - "no CUDA needed" stays
        # true for anyone who only wants the app and the existing tap. The
        # bundle that ships is the one built here.
        # ------------------------------------------------------------------
        target_sources(ScopeTap PRIVATE core/ScopeCuda.cu core/ScopeCuda.h)
        target_compile_definitions(ScopeTap PRIVATE SCOPE_TAP_CUDA)
        set_target_properties(ScopeTap PROPERTIES
            CUDA_ARCHITECTURES "${SPIKE_CUDA_ARCHS}"
            CUDA_SEPARABLE_COMPILATION OFF
            CUDA_RUNTIME_LIBRARY Static)
        # -fmad=false for the same reason the conformance target needs it: a
        # contracted luma dot product lands boundary values in different bins
        # from the CPU, and the whole acceptance argument rests on the two
        # agreeing bit-for-bit.
        target_compile_options(ScopeTap PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-fmad=false>
            $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>)

        # scope_conformance_cuda - the same harness with the CUDA backend
        # compiled in, so the kernel can be judged without Resolve in the loop.
        # Correctness is settled here, in seconds, over the full matrix; only
        # integration needs a running host.
        #
        # -fmad=false is not optional. nvcc contracts a*b+c by default, and the
        # luma dot product then lands boundary values in different bins from the
        # CPU - which looks exactly like the coverage bug this harness exists to
        # find. The harness also refuses to run a contracted build, but the flag
        # is what stops it happening.
        add_executable(scope_conformance_cuda
            tools/ScopeConformance.cpp
            core/ScopeCuda.cu
            core/ScopeCuda.h)
        target_link_libraries(scope_conformance_cuda PRIVATE scopecore_writer)
        target_include_directories(scope_conformance_cuda PRIVATE core)
        target_compile_definitions(scope_conformance_cuda PRIVATE SCOPE_HAVE_CUDA)
        set_target_properties(scope_conformance_cuda PROPERTIES
            CUDA_ARCHITECTURES "${SPIKE_CUDA_ARCHS}"
            CUDA_SEPARABLE_COMPILATION OFF
            CUDA_RUNTIME_LIBRARY Static)
        target_compile_options(scope_conformance_cuda PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-fmad=false>
            $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>)
        if(MSVC)
            target_compile_options(scope_conformance_cuda PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:/fp:precise>)
            target_compile_definitions(scope_conformance_cuda PRIVATE
                NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()

        # scope_publish_race_cuda - the gate above drives CudaReducer, which is
        # synchronous and cannot see frames overlapping. This drives the real
        # GpuTap::RenderFrame and publish hub into a scratch block and checks
        # every slot carries its own frame (see the file's header). ScopeCuda.cu
        # is compiled into that file, not listed here, because the hub is
        # internal to it.
        add_executable(scope_publish_race_cuda tools/ScopePublishRace.cu)
        target_link_libraries(scope_publish_race_cuda PRIVATE scopecore_writer)
        target_include_directories(scope_publish_race_cuda PRIVATE core)
        set_target_properties(scope_publish_race_cuda PROPERTIES
            CUDA_ARCHITECTURES "${SPIKE_CUDA_ARCHS}"
            CUDA_SEPARABLE_COMPILATION OFF
            CUDA_RUNTIME_LIBRARY Static)
        target_compile_options(scope_publish_race_cuda PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-fmad=false>
            $<$<COMPILE_LANGUAGE:CUDA>:-Wno-deprecated-gpu-targets>)
        if(MSVC)
            target_compile_definitions(scope_publish_race_cuda PRIVATE
                NOMINMAX _CRT_SECURE_NO_WARNINGS)
        endif()

        add_custom_command(TARGET ScopeTapGpuSpike POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${SPIKE_BUNDLE_DIR}/Contents/${OFX_BUNDLE_ARCH_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${SPIKE_BUNDLE_DIR}/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:ScopeTapGpuSpike>"
                    "${SPIKE_BUNDLE_DIR}/Contents/${OFX_BUNDLE_ARCH_DIR}/ScopeTapGpuSpike.ofx"
            COMMENT "Packaging ScopeTapGpuSpike.ofx.bundle"
            VERBATIM)
    endif()
endif()

