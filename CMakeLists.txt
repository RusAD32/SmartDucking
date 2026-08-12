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

juce_add_plugin(SmartDucking
    VERSION 1.0.0
    COMPANY_NAME "OnWave"
    COMPANY_WEBSITE "https://onwavedub.org"
    BUNDLE_ID "org.onwavedub.SmartDucking"
    PLUGIN_MANUFACTURER_CODE OnWv
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
        Resources/OnWaveLogo.png
)

target_link_libraries(SmartDucking
    PRIVATE
        SmartDuckingResources
)

target_sources(SmartDucking PRIVATE
    Source/ParameterIds.h
    Source/DiagnosticLogger.h
    Source/DiagnosticLogger.cpp
    Source/FactoryPresets.h
    Source/LcrDuckMatrix.h
    Source/LinkwitzRiley8Crossover.h
    Source/PceHistory.h
    Source/PluginVariant.h
    Source/SidechainClassifier.h
    Source/SidechainEnvelope.h
    Source/SpectralFrameSmoothers.h
    Source/SpectralSuppressor.h
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp)

target_include_directories(SmartDucking PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

target_compile_definitions(SmartDucking PUBLIC
    SMART_DUCKING_ENABLE_DIAGNOSTICS=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SmartDucking
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
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
        Source/DiagnosticLogger.h
        Source/DiagnosticLogger.cpp
        Source/FactoryPresets.h
        Source/LcrDuckMatrix.h
        Source/LinkwitzRiley8Crossover.h
        Source/PceHistory.h
        Source/PluginVariant.h
        Source/SidechainClassifier.h
        Source/SidechainEnvelope.h
        Source/SpectralFrameSmoothers.h
        Source/SpectralSuppressor.h
        Source/PluginEditor.h
        Source/PluginEditor.cpp
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp)

    target_include_directories(SmartDuckingTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

    target_compile_definitions(SmartDuckingTests PRIVATE
        SMART_DUCKING_ENABLE_DIAGNOSTICS=0
        JucePlugin_Name="Smart Ducking"
        JucePlugin_IsSynth=0
        JucePlugin_Enable_ARA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(SmartDuckingTests PRIVATE
        SmartDuckingResources
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    add_test(NAME SmartDucking.Processor COMMAND SmartDuckingTests)

    juce_add_console_app(SmartDuckingBenchmark
        PRODUCT_NAME "Smart Ducking DSP Benchmark")
    target_sources(SmartDuckingBenchmark PRIVATE Tests/PerformanceBenchmark.cpp)
    target_include_directories(SmartDuckingBenchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(SmartDuckingBenchmark PRIVATE
        SMART_DUCKING_ENABLE_DIAGNOSTICS=0
        JucePlugin_Name="Smart Ducking"
        JucePlugin_IsSynth=0
        JucePlugin_Enable_ARA=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(SmartDuckingBenchmark PRIVATE
        SmartDucking
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
