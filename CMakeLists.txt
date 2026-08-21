cmake_minimum_required(VERSION 3.22)

project(SmartDucking VERSION 1.0.0 LANGUAGES C CXX)

if (APPLE)
    enable_language(OBJCXX)

    # JUCE always adds conditional ARA/LV2 translation units. They are
    # intentionally empty for this VST3-only build, so suppress only Apple's
    # harmless "has no symbols" ranlib diagnostic for static archives.
    set(CMAKE_CXX_ARCHIVE_FINISH
        "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SMART_DUCKING_USE_PFFFT
    "Use PFFFT instead of juce::dsp::FFT for real-time spectral processing"
    ON)
option(SMART_DUCKING_ENABLE_DIAGNOSTICS
    "Enable detailed DSP stage timing and diagnostic logs"
    OFF)
option(SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH
    "Enable the allocation-free active steady-state block path"
    ON)
option(SMART_DUCKING_USE_XSIMD
    "Use xsimd vector math in the spectral mask and gain kernels"
    ON)
set(PFFFT_PATH "" CACHE PATH "Optional path to a local PFFFT source tree")
set(XSIMD_PATH "" CACHE PATH "Optional path to a local xsimd source tree")

set(JUCE_PATH "$ENV{JUCE_PATH}" CACHE PATH "Path to the JUCE source tree")

# Reuse the workspace-level JUCE checkout when this repository is kept under
# vst/src. A standalone clone falls back to the pinned upstream release below.
if (NOT JUCE_PATH)
    set(smart_ducking_workspace_juce
        "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE-8.0.12")

    if (EXISTS "${smart_ducking_workspace_juce}/CMakeLists.txt")
        set(JUCE_PATH "${smart_ducking_workspace_juce}")
    endif()
endif()

if (JUCE_PATH)
    if (NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
        message(FATAL_ERROR "JUCE_PATH does not point to a JUCE source tree: ${JUCE_PATH}")
    endif()
    add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    find_package(JUCE CONFIG QUIET)

    if (NOT TARGET juce::juce_core)
        include(FetchContent)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.12
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()
endif()

set(smart_ducking_fft_link_target "")

if (SMART_DUCKING_USE_PFFFT)
    set(PFFFT_USE_TYPE_FLOAT ON CACHE BOOL "" FORCE)
    set(PFFFT_USE_TYPE_DOUBLE OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(INSTALL_PFFFT OFF CACHE BOOL "" FORCE)
    set(INSTALL_PFDSP OFF CACHE BOOL "" FORCE)
    set(INSTALL_PFFASTCONV OFF CACHE BOOL "" FORCE)

    if (PFFFT_PATH)
        if (NOT EXISTS "${PFFFT_PATH}/CMakeLists.txt")
            message(FATAL_ERROR
                "PFFFT_PATH does not point to a PFFFT source tree: ${PFFFT_PATH}")
        endif()

        add_subdirectory("${PFFFT_PATH}" "${CMAKE_BINARY_DIR}/PFFFT" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(PFFFT
            GIT_REPOSITORY https://github.com/marton78/pffft.git
            GIT_TAG a4b03590cc2a4bea56f9721996e3057835799179)
        FetchContent_MakeAvailable(PFFFT)
    endif()

    if (NOT TARGET PFFFT::PFFFT)
        message(FATAL_ERROR "PFFFT::PFFFT target is unavailable")
    endif()

    # The maintained PFFFT fork requires this definition to select its NEON
    # implementation. It is harmless on x86, where the SSE path is selected.
    target_compile_definitions(PFFFT PRIVATE PFFFT_ENABLE_NEON=1)
    set_target_properties(PFFFT PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(smart_ducking_fft_link_target PFFFT::PFFFT)
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(XSIMD_SKIP_INSTALL ON CACHE BOOL "" FORCE)

if (XSIMD_PATH)
    if (NOT EXISTS "${XSIMD_PATH}/CMakeLists.txt")
        message(FATAL_ERROR
            "XSIMD_PATH does not point to an xsimd source tree: ${XSIMD_PATH}")
    endif()

    add_subdirectory("${XSIMD_PATH}" "${CMAKE_BINARY_DIR}/xsimd" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(xsimd
        GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
        GIT_TAG e88a72831858123924f7118f345dfe5d70d95991)
    FetchContent_MakeAvailable(xsimd)
endif()

if (NOT TARGET xsimd::xsimd)
    message(FATAL_ERROR "xsimd::xsimd target is unavailable")
endif()

set(smart_ducking_has_xsimd_avx2_kernel OFF)

if (MSVC)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(smart_ducking_has_xsimd_avx2_kernel ON)
        set_source_files_properties(Source/SpectralVectorMathAvx2.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    endif()
elseif (APPLE)
    list(FIND CMAKE_OSX_ARCHITECTURES "x86_64" smart_ducking_x86_64_arch_index)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$"
        OR NOT smart_ducking_x86_64_arch_index EQUAL -1)
        set(smart_ducking_has_xsimd_avx2_kernel ON)
        set_source_files_properties(Source/SpectralVectorMathAvx2.cpp
            PROPERTIES COMPILE_OPTIONS "-Xarch_x86_64;-mavx2")
    endif()
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
    set(smart_ducking_has_xsimd_avx2_kernel ON)
    set_source_files_properties(Source/SpectralVectorMathAvx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

add_library(SmartDuckingVectorMath STATIC
    Source/SpectralVectorMath.h
    Source/SpectralVectorMathKernels.h
    Source/SpectralVectorMath.cpp)

if (smart_ducking_has_xsimd_avx2_kernel)
    target_sources(SmartDuckingVectorMath PRIVATE
        Source/SpectralVectorMathAvx2.cpp)
endif()

set_target_properties(SmartDuckingVectorMath PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(SmartDuckingVectorMath PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_features(SmartDuckingVectorMath PUBLIC cxx_std_20)
target_compile_definitions(SmartDuckingVectorMath PRIVATE
    SMART_DUCKING_HAS_XSIMD_AVX2_KERNEL=$<BOOL:${smart_ducking_has_xsimd_avx2_kernel}>
    SMART_DUCKING_USE_XSIMD=$<BOOL:${SMART_DUCKING_USE_XSIMD}>)
target_link_libraries(SmartDuckingVectorMath PUBLIC xsimd::xsimd)

juce_add_plugin(SmartDucking
    VERSION 1.0.0
    COMPANY_NAME "RusAD"
    COMPANY_WEBSITE "https://github.com/RusAD32"
    BUNDLE_ID "org.rusad.SmartDucking"
    PLUGIN_MANUFACTURER_CODE RsAD
    PLUGIN_CODE Smdk
    FORMATS VST3 Standalone
    PRODUCT_NAME "Smart Ducking"
    DESCRIPTION "Voice-aware spatial and spectral ducking for voice-over mixing"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    VST3_CATEGORIES Fx Dynamics)

juce_add_binary_data(SmartDuckingResources
    NAMESPACE BinaryData
    SOURCES
        Resources/RusADLogo.svg
)

target_link_libraries(SmartDucking
    PRIVATE
        SmartDuckingResources
)

target_sources(SmartDucking PRIVATE
    Source/ParameterIds.h
    Source/CrossoverMagnitude.h
    Source/DiagnosticLogger.h
    Source/DiagnosticLogger.cpp
    Source/FactoryPresets.h
    Source/UserPresetStore.h
    Source/UserPresetStore.cpp
    Source/LcrDuckMatrix.h
    Source/LinkwitzRileyCrossover.h
    Source/PceHistory.h
    Source/PowerOfTwoDelayBuffer.h
    Source/RealFft.h
    Source/PluginVariant.h
    Source/SidechainClassifier.h
    Source/SidechainEnvelope.h
    Source/SpectralFrameSmoothers.h
    Source/SpectralVectorMath.h
    Source/SpectralSuppressor.h
    Source/SurroundChannelOrder.h
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp)

target_include_directories(SmartDucking PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

target_compile_definitions(SmartDucking PUBLIC
    SMART_DUCKING_ENABLE_DIAGNOSTICS=$<BOOL:${SMART_DUCKING_ENABLE_DIAGNOSTICS}>
    SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH=$<BOOL:${SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH}>
    SMART_DUCKING_USE_PFFFT=$<BOOL:${SMART_DUCKING_USE_PFFFT}>
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SmartDucking
    PRIVATE
        SmartDuckingVectorMath
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        ${smart_ducking_fft_link_target}
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# JUCE's stock VST3 client exports a single audio-module class. Replace only
# that wrapper source with a small composite factory which exposes Stereo and
# Surround components from the same binary. The shared-code target above still
# owns all processor, DSP, state, and editor implementation.
get_target_property(smart_ducking_vst3_links SmartDucking_VST3 LINK_LIBRARIES)
list(FILTER smart_ducking_vst3_links EXCLUDE REGEX "juce_audio_plugin_client_VST3")
set_property(TARGET SmartDucking_VST3 PROPERTY LINK_LIBRARIES "${smart_ducking_vst3_links}")
target_sources(SmartDucking_VST3 PRIVATE Source/SmartDuckingVST3Client.cpp)

# The JUCE VST3 wrapper contains Objective-C++ code on macOS. Keep the .cpp
# filename for the existing Windows build, but compile it as Objective-C++ on
# Apple platforms.
if (APPLE)
    set_source_files_properties(
        Source/SmartDuckingVST3Client.cpp
        PROPERTIES LANGUAGE OBJCXX)
endif()

target_compile_definitions(SmartDucking_VST3 PRIVATE
    JucePlugin_Build_AU=0
    JucePlugin_Build_AUv3=0
    JucePlugin_Build_AAX=0
    JucePlugin_Build_LV2=0
    JucePlugin_Build_Standalone=0
    JucePlugin_Build_Unity=0
    JucePlugin_Build_VST=0
    JucePlugin_Build_VST3=1)

configure_file(
    Resources/moduleinfo.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/SmartDucking-moduleinfo.json"
    @ONLY)

add_custom_command(TARGET SmartDucking_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:SmartDucking_VST3>/../Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
        "$<TARGET_FILE_DIR:SmartDucking_VST3>/../Resources/THIRD_PARTY_LICENSES.md"
    # JUCE's manifest helper only knows about the stock component. Replace its
    # output with the manifest for our two-class composite VST3 factory.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/SmartDucking-moduleinfo.json"
        "$<TARGET_FILE_DIR:SmartDucking_VST3>/../Resources/moduleinfo.json"
    VERBATIM)

add_custom_command(TARGET SmartDucking_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
        "$<TARGET_FILE_DIR:SmartDucking_Standalone>/THIRD_PARTY_LICENSES.md"
    VERBATIM)

# JUCE creates an ad-hoc signature before its manifest helper and the custom
# resource-copy steps above run. Sign the completed bundles once more so the
# final resources are covered by the signature.
if (APPLE)
    add_custom_command(TARGET SmartDucking_VST3 POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign -
            "$<TARGET_BUNDLE_DIR:SmartDucking_VST3>"
        VERBATIM)

    add_custom_command(TARGET SmartDucking_Standalone POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign -
            "$<TARGET_BUNDLE_DIR:SmartDucking_Standalone>"
        VERBATIM)
endif()

option(SMART_DUCKING_BUILD_TESTS "Build the Smart Ducking scaffold tests" ON)
set(SMART_DUCKING_PLUGINVAL_EXECUTABLE "" CACHE FILEPATH
    "Optional pluginval executable used by the SmartDuckingPluginval target")

if (SMART_DUCKING_PLUGINVAL_EXECUTABLE)
    if (NOT EXISTS "${SMART_DUCKING_PLUGINVAL_EXECUTABLE}")
        message(FATAL_ERROR
            "SMART_DUCKING_PLUGINVAL_EXECUTABLE does not exist: ${SMART_DUCKING_PLUGINVAL_EXECUTABLE}")
    endif()

    set(smart_ducking_pluginval_output "${CMAKE_CURRENT_BINARY_DIR}/pluginval-results")
    get_target_property(smart_ducking_pluginval_target
        SmartDucking_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
    if (NOT smart_ducking_pluginval_target)
        message(FATAL_ERROR
            "JUCE_PLUGIN_ARTEFACT_FILE is not set for SmartDucking_VST3")
    endif()

    add_custom_target(SmartDuckingPluginval
        COMMAND ${CMAKE_COMMAND} -E make_directory "${smart_ducking_pluginval_output}"
        COMMAND "${SMART_DUCKING_PLUGINVAL_EXECUTABLE}"
            --strictness-level 5
            --validate-in-process
            --output-dir "${smart_ducking_pluginval_output}"
            "${smart_ducking_pluginval_target}"
        DEPENDS SmartDucking_VST3
        USES_TERMINAL
        VERBATIM)
endif()

if (SMART_DUCKING_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(SmartDuckingTests PRODUCT_NAME "Smart Ducking Tests")

    target_sources(SmartDuckingTests PRIVATE
        Tests/ProcessorTests.cpp
        Source/ParameterIds.h
        Source/CrossoverMagnitude.h
        Source/DiagnosticLogger.h
        Source/DiagnosticLogger.cpp
        Source/FactoryPresets.h
        Source/UserPresetStore.h
        Source/UserPresetStore.cpp
        Source/LcrDuckMatrix.h
        Source/LinkwitzRileyCrossover.h
        Source/PceHistory.h
        Source/PowerOfTwoDelayBuffer.h
        Source/RealFft.h
        Source/PluginVariant.h
        Source/SidechainClassifier.h
        Source/SidechainEnvelope.h
        Source/SpectralFrameSmoothers.h
        Source/SpectralVectorMath.h
        Source/SpectralSuppressor.h
        Source/SurroundChannelOrder.h
        Source/PluginEditor.h
        Source/PluginEditor.cpp
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp)

    target_include_directories(SmartDuckingTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

    target_compile_definitions(SmartDuckingTests PRIVATE
        SMART_DUCKING_ENABLE_DIAGNOSTICS=$<BOOL:${SMART_DUCKING_ENABLE_DIAGNOSTICS}>
        SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH=$<BOOL:${SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH}>
        SMART_DUCKING_ENABLE_TEST_HOOKS=1
        SMART_DUCKING_USE_PFFFT=$<BOOL:${SMART_DUCKING_USE_PFFFT}>
        JucePlugin_Name="Smart Ducking"
        JucePlugin_IsSynth=0
        JucePlugin_Enable_ARA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(SmartDuckingTests PRIVATE
        SmartDuckingResources
        SmartDuckingVectorMath
        ${smart_ducking_fft_link_target}
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    if (MSVC)
        # The off-screen editor test creates a native JUCE image. On Windows,
        # that initialises Direct3D, and some GPU drivers need more stack than
        # the 1 MiB console default after this test's large fixture frame.
        # This changes only the test executable, never the plug-in binary.
        target_link_options(SmartDuckingTests PRIVATE /STACK:8388608)
    endif()

    add_test(NAME SmartDucking.Processor COMMAND SmartDuckingTests)

    juce_add_console_app(SmartDuckingBenchmark
        PRODUCT_NAME "Smart Ducking DSP Benchmark")
    target_sources(SmartDuckingBenchmark PRIVATE Tests/PerformanceBenchmark.cpp)
    target_include_directories(SmartDuckingBenchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(SmartDuckingBenchmark PRIVATE
        SMART_DUCKING_ENABLE_DIAGNOSTICS=$<BOOL:${SMART_DUCKING_ENABLE_DIAGNOSTICS}>
        JucePlugin_Name="Smart Ducking"
        JucePlugin_IsSynth=0
        JucePlugin_Enable_ARA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(SmartDuckingBenchmark PRIVATE
        SmartDucking
        SmartDuckingVectorMath
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    if (MSVC)
        target_compile_options(SmartDuckingBenchmark PRIVATE /wd4005)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(SmartDuckingBenchmark PRIVATE -Wno-macro-redefined)
    endif()

    if (WIN32 OR UNIX)
        add_executable(SmartDuckingVST3FactoryTests)

        target_sources(SmartDuckingVST3FactoryTests PRIVATE
            Tests/VST3FactoryTests.cpp
            "${JUCE_MODULES_DIR}/juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/coreiids.cpp"
            "${JUCE_MODULES_DIR}/juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/vst/vstinitiids.cpp")

        # This test is deliberately a plain VST3 SDK client. Linking JUCE into
        # both the test process and the dynamically loaded plug-in creates two
        # independent copies of JUCE's global state on macOS.
        target_include_directories(SmartDuckingVST3FactoryTests PRIVATE
            "${JUCE_MODULES_DIR}/juce_audio_processors_headless/format_types/VST3_SDK")

        if (APPLE)
            target_link_libraries(SmartDuckingVST3FactoryTests PRIVATE
                "-framework CoreFoundation")
        elseif (UNIX)
            target_link_libraries(SmartDuckingVST3FactoryTests PRIVATE
                ${CMAKE_DL_LIBS})
        endif()

        add_dependencies(SmartDuckingVST3FactoryTests SmartDucking_VST3)

        add_test(NAME SmartDucking.VST3Factory
            COMMAND SmartDuckingVST3FactoryTests "$<TARGET_FILE:SmartDucking_VST3>")
    endif()
endif()
