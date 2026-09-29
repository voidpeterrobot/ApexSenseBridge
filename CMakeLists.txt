cmake_minimum_required(VERSION 3.20)
project(ApexSenseBridge VERSION 0.6.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/EHsc)
    # Ship a self-contained native runtime and let the linker discard unused
    # code across the static core library. End users no longer need the VC++
    # redistributable solely for ApexSenseBridge.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

add_library(asb_core STATIC
    src/core/ApexProfileRestoreGuard.cpp
    src/diagnostics/HidDiagnostics.cpp
    src/dualsense/DualSenseFeedback.cpp
    src/dualsense/DualSenseFirmware.cpp
    src/dualsense/DualSenseInput.cpp
    src/dualsense/TouchpadGestureProfile.cpp
    src/dualsense/ViewTouchpadGesture.cpp
    src/dualsense/AdaptiveTriggerTranslation.cpp
    src/dualsense/AdaptiveTriggerBridge.cpp
    src/dualsense/RumbleBridge.cpp
    src/dualsense/ViiperProtocol.cpp
    src/haptics/HapticProcessor.cpp
    src/platform/AudioEndpointProtection.cpp
    src/platform/PhysicalInputSource.h
    src/platform/SessionControl.cpp
    src/platform/XInputMapping.cpp
    src/flydigi/Apex4Input.cpp
    src/flydigi/Apex4Protocol.cpp
    src/flydigi/Apex5Identity.cpp
    src/flydigi/Apex5Protocol.cpp
    src/flydigi/Apex5Device.cpp
)
target_include_directories(asb_core PUBLIC src)

if(WIN32)
    target_sources(asb_core PRIVATE
        src/platform/windows/WindowsHidTransport.cpp
        src/platform/windows/WindowsHidDiscovery.cpp
        src/platform/windows/WindowsGenericHidInputSource.cpp
        src/platform/windows/WindowsAudioEndpointProtection.cpp
        src/platform/windows/WindowsLibViiperVirtualDualSense.cpp
        src/platform/windows/WindowsVirtualDualSense.cpp
        src/platform/windows/WindowsXInputGamepad.cpp
        src/platform/windows/WindowsPhysicalControllerIsolation.cpp
        src/platform/windows/WindowsPhysicalInputSource.cpp
        src/platform/windows/WindowsSessionControl.cpp
        src/platform/windows/WindowsApex6Settings.cpp
        src/platform/windows/WindowsGripControl.cpp
    )
    target_link_libraries(asb_core PRIVATE setupapi hid cfgmgr32 ws2_32 xinput9_1_0 ole32)
else()
    target_sources(asb_core PRIVATE
        src/platform/UnsupportedHidTransport.cpp
        src/platform/UnsupportedAudioEndpointProtection.cpp
        src/platform/UnsupportedVirtualDualSense.cpp
        src/platform/UnsupportedXInputGamepad.cpp
        src/platform/UnsupportedPhysicalControllerIsolation.cpp
        src/platform/UnsupportedPhysicalInputSource.cpp
        src/platform/UnsupportedSessionControl.cpp
    )
endif()

if(WIN32)
    set(ASB_RC_FILE src/platform/windows/ApexSenseBridge.rc)
endif()

add_executable(ApexSenseBridge
    src/main.cpp
    src/cli/CliApplication.cpp
    src/cli/CommandSupport.cpp
    src/cli/DeviceCommands.cpp
    src/cli/VirtualDualSenseCommand.cpp
    src/cli/BridgeCommand.cpp
    ${ASB_RC_FILE}
)
target_link_libraries(ApexSenseBridge PRIVATE asb_core)
if(WIN32)
    target_link_libraries(ApexSenseBridge PRIVATE psapi avrt)
    target_sources(ApexSenseBridge PRIVATE src/cli/Apex6BridgeCommand.cpp)
    target_compile_definitions(ApexSenseBridge PRIVATE
        ASB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ASB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ASB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        ASB_VERSION_STRING=\"${PROJECT_VERSION}\"
    )
endif()

if(WIN32)
    add_executable(ApexSenseBridgeControl WIN32 src/control/ControlPanel.cpp ${ASB_RC_FILE})
    add_executable(ApexSenseBridgeIsolationProbe src/platform/windows/IsolationProbe.cpp ${ASB_RC_FILE})
    target_link_libraries(ApexSenseBridgeIsolationProbe PRIVATE xinput9_1_0)
    target_compile_definitions(ApexSenseBridgeIsolationProbe PRIVATE
        ASB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ASB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ASB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        ASB_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(ApexSenseBridgeControl PRIVATE user32 shell32 advapi32)
    target_compile_definitions(ApexSenseBridgeControl PRIVATE
        ASB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ASB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ASB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        ASB_VERSION_STRING=\"${PROJECT_VERSION}\"
    )
endif()

if(MSVC)
    target_compile_options(asb_core PRIVATE /W4 /permissive- /EHsc /Gy $<$<CONFIG:Release>:/O2>)
    target_compile_options(ApexSenseBridge PRIVATE /W4 /permissive- /EHsc /Gy $<$<CONFIG:Release>:/O2>)
    target_compile_options(ApexSenseBridge PRIVATE /utf-8)
    if(WIN32)
        target_compile_options(ApexSenseBridgeControl PRIVATE /W4 /permissive- /EHsc /Gy /utf-8 $<$<CONFIG:Release>:/O2>)
    endif()
else()
    target_compile_options(asb_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ApexSenseBridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

option(ASB_BUILD_APEX6_EXPERIMENTS "Build isolated query-only Apex6 transport and offline neutral rehearsal" OFF)
option(ASB_BUILD_APEX6_NEUTRAL_EXPERIMENTS "Build separately approved physical neutral-only Apex6 experiment (not packaged)" OFF)
option(ASB_BUILD_APEX6_LIVE_BRIDGE "Build experimental supervised Apex6 live grip bridge (not packaged)" OFF)
option(ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS "Debug live grips: record but do not reject late dispatch (not packaged)" OFF)
if(ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS AND NOT ASB_BUILD_APEX6_LIVE_BRIDGE)
    message(FATAL_ERROR "Debug dispatch policy requires the experimental live bridge")
endif()
if(ASB_BUILD_APEX6_LIVE_BRIDGE AND NOT ASB_BUILD_APEX6_EXPERIMENTS)
    message(FATAL_ERROR "Live bridge requires ASB_BUILD_APEX6_EXPERIMENTS")
endif()
if(ASB_BUILD_APEX6_NEUTRAL_EXPERIMENTS AND NOT ASB_BUILD_APEX6_EXPERIMENTS)
    message(FATAL_ERROR "Neutral experiment requires ASB_BUILD_APEX6_EXPERIMENTS")
endif()
    add_library(asb_apex6_session STATIC src/apex6/experiment/Session.cpp src/apex6/experiment/Evidence.cpp src/apex6/experiment/Pulse.cpp src/apex6/experiment/PulseEvidence.cpp)
    target_include_directories(asb_apex6_session PUBLIC src)
    target_link_libraries(asb_apex6_session PUBLIC asb_apex6_offline)
    add_library(asb_apex6_fake STATIC src/apex6/experiment/FakeIo.cpp)
    target_link_libraries(asb_apex6_fake PUBLIC asb_apex6_session)
    if(MSVC)
        target_compile_options(asb_apex6_session PRIVATE /W4 /permissive- /EHsc)
        target_compile_options(asb_apex6_fake PRIVATE /W4 /permissive- /EHsc)
    endif()
if(ASB_BUILD_APEX6_EXPERIMENTS)
    if(WIN32)
        add_library(asb_apex6_query_windows STATIC src/apex6/experiment/WindowsIo.cpp src/apex6/experiment/WindowsSessionLock.cpp src/platform/windows/WindowsHidDiscovery.cpp)
        target_link_libraries(asb_apex6_query_windows PUBLIC asb_apex6_session PRIVATE hid setupapi cfgmgr32 ole32 bcrypt)
        if(MSVC)
            target_compile_options(asb_apex6_query_windows PRIVATE /W4 /permissive- /EHsc)
        endif()
        file(GLOB ASB_APEX6_HASH_INPUTS CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/src/apex6/*.h" "${CMAKE_SOURCE_DIR}/src/apex6/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/apex6/experiment/*.h" "${CMAKE_SOURCE_DIR}/src/apex6/experiment/*.cpp")
        file(GLOB_RECURSE ASB_LIVE_HASH_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.h" "${CMAKE_SOURCE_DIR}/src/*.cpp")
        list(APPEND ASB_APEX6_HASH_INPUTS ${ASB_LIVE_HASH_INPUTS})
        if(ASB_BUILD_APEX6_LIVE_BRIDGE)
            list(APPEND ASB_APEX6_HASH_INPUTS "${CMAKE_SOURCE_DIR}/scripts/Invoke-Apex6LiveBridge.ps1" "${CMAKE_SOURCE_DIR}/scripts/Apex6HidHide.ps1" "${CMAKE_SOURCE_DIR}/scripts/Watch-Apex6HidHide.ps1" "${CMAKE_SOURCE_DIR}/scripts/Test-Apex6XInputIsolation.ps1" "${CMAKE_SOURCE_DIR}/tests/apex6_live_fixture.h")
        endif()
        list(REMOVE_DUPLICATES ASB_APEX6_HASH_INPUTS)
        list(APPEND ASB_APEX6_HASH_INPUTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/apex6/OfficialSilent.h"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/apex6/OfficialSilent.json"
            "${CMAKE_SOURCE_DIR}/scripts/Invoke-Apex6GripLifecycleTest.ps1"
            "${CMAKE_SOURCE_DIR}/scripts/Invoke-Apex6GripPulseTest.ps1"
            "${CMAKE_SOURCE_DIR}/docs/fixtures/grip-left-pulse-v1.json"
            "${CMAKE_SOURCE_DIR}/docs/fixtures/grip-left-pulse-v2.json"
            "${CMAKE_SOURCE_DIR}/docs/fixtures/grip-left-pulse-v3.json"
            "${CMAKE_SOURCE_DIR}/docs/fixtures/grip-left-pulse-v4.json"
            "${CMAKE_SOURCE_DIR}/scripts/Show-Apex6ExperimentDiagnostics.ps1"
            "${CMAKE_SOURCE_DIR}/src/platform/windows/WindowsHidDiscovery.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/HidDiscovery.h" "${CMAKE_SOURCE_DIR}/src/core/DeviceInfo.h"
            "${CMAKE_SOURCE_DIR}/tests/apex6_rehearsal_fixture.h" "${CMAKE_SOURCE_DIR}/tests/fixtures/apex6/ReferenceVectors.h")
        list(SORT ASB_APEX6_HASH_INPUTS)
        set(ASB_APEX6_HASH_TEXT "")
        foreach(input IN LISTS ASB_APEX6_HASH_INPUTS)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
            file(SHA256 "${input}" input_hash)
            file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${input}")
            string(APPEND ASB_APEX6_HASH_TEXT "${relative}:${input_hash}\n")
        endforeach()
        string(APPEND ASB_APEX6_HASH_TEXT "live_debug_no_dispatch_lateness=${ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS}\n")
        string(SHA256 ASB_APEX6_SOURCE_DIGEST "${ASB_APEX6_HASH_TEXT}")
        file(WRITE "${CMAKE_BINARY_DIR}/apex6-source-hashes.txt" "${ASB_APEX6_HASH_TEXT}")
        configure_file(src/apex6/experiment/BuildIdentity.h.in generated/BuildIdentity.h @ONLY)
        add_executable(ApexSenseBridgeApex6Experiment src/apex6/experiment/main.cpp)
        target_include_directories(ApexSenseBridgeApex6Experiment PRIVATE "${CMAKE_BINARY_DIR}/generated" tests)
        target_link_libraries(ApexSenseBridgeApex6Experiment PRIVATE asb_apex6_query_windows asb_apex6_fake)
        if(MSVC)
            target_compile_options(ApexSenseBridgeApex6Experiment PRIVATE /W4 /permissive- /EHsc)
        endif()
        if(ASB_BUILD_APEX6_NEUTRAL_EXPERIMENTS)
            add_library(asb_apex6_neutral_windows STATIC src/apex6/experiment/WindowsIo.cpp src/apex6/experiment/WindowsSessionLock.cpp src/platform/windows/WindowsHidDiscovery.cpp)
            target_compile_definitions(asb_apex6_neutral_windows PUBLIC ASB_APEX6_NEUTRAL_RUNNER=1)
            target_link_libraries(asb_apex6_neutral_windows PUBLIC asb_apex6_session PRIVATE hid setupapi cfgmgr32 ole32 bcrypt)
            add_executable(ApexSenseBridgeApex6NeutralExperiment src/apex6/experiment/main.cpp)
            target_include_directories(ApexSenseBridgeApex6NeutralExperiment PRIVATE "${CMAKE_BINARY_DIR}/generated" tests)
            target_link_libraries(ApexSenseBridgeApex6NeutralExperiment PRIVATE asb_apex6_neutral_windows asb_apex6_fake)
            if(MSVC)
                target_compile_options(asb_apex6_neutral_windows PRIVATE /W4 /permissive- /EHsc)
                target_compile_options(ApexSenseBridgeApex6NeutralExperiment PRIVATE /W4 /permissive- /EHsc)
            endif()
        endif()
    endif()
    if(BUILD_TESTING)
        add_executable(ApexSenseBridgeApex6ExperimentTests tests/test_apex6_experiment.cpp)
        target_link_libraries(ApexSenseBridgeApex6ExperimentTests PRIVATE asb_apex6_session asb_apex6_fake)
        add_test(NAME apex6_experiment_faults COMMAND ApexSenseBridgeApex6ExperimentTests)
        add_executable(ApexSenseBridgeApex6PulseTests tests/test_apex6_pulse.cpp)
        target_link_libraries(ApexSenseBridgeApex6PulseTests PRIVATE asb_apex6_session asb_apex6_fake)
        add_test(NAME apex6_grip_pulse_faults COMMAND ApexSenseBridgeApex6PulseTests)
        if(WIN32)
            add_executable(ApexSenseBridgeApex6PulseTimingProbe tests/apex6_pulse_timing_probe.cpp)
            target_link_libraries(ApexSenseBridgeApex6PulseTimingProbe PRIVATE asb_apex6_session asb_apex6_fake)
            add_executable(ApexSenseBridgeApex6WindowsIoTests tests/test_apex6_windows_io.cpp)
            target_link_libraries(ApexSenseBridgeApex6WindowsIoTests PRIVATE asb_apex6_query_windows)
            add_test(NAME apex6_windows_query_io COMMAND ApexSenseBridgeApex6WindowsIoTests)
            add_executable(ApexSenseBridgeApex6LockTests tests/test_apex6_lock.cpp)
            target_link_libraries(ApexSenseBridgeApex6LockTests PRIVATE asb_apex6_query_windows ole32)
            add_test(NAME apex6_experiment_lock COMMAND ApexSenseBridgeApex6LockTests)
            if(ASB_BUILD_APEX6_NEUTRAL_EXPERIMENTS)
                add_executable(ApexSenseBridgeApex6NeutralIoTests tests/test_apex6_windows_io.cpp)
                target_link_libraries(ApexSenseBridgeApex6NeutralIoTests PRIVATE asb_apex6_neutral_windows)
                add_test(NAME apex6_windows_neutral_io COMMAND ApexSenseBridgeApex6NeutralIoTests)
                add_test(NAME apex6_neutral_cli COMMAND powershell -NoProfile -NonInteractive
                    -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_neutral_cli.ps1"
                    -Executable "$<TARGET_FILE:ApexSenseBridgeApex6NeutralExperiment>"
                    -QueryExecutable "$<TARGET_FILE:ApexSenseBridgeApex6Experiment>")
                add_test(NAME apex6_grip_lifecycle_cli COMMAND powershell -NoProfile -NonInteractive
                    -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_lifecycle_cli.ps1"
                    -Executable "$<TARGET_FILE:ApexSenseBridgeApex6NeutralExperiment>"
                    -QueryExecutable "$<TARGET_FILE:ApexSenseBridgeApex6Experiment>")
                add_test(NAME apex6_grip_pulse_cli COMMAND powershell -NoProfile -NonInteractive
                    -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_pulse_cli.ps1"
                    -Executable "$<TARGET_FILE:ApexSenseBridgeApex6NeutralExperiment>"
                    -QueryExecutable "$<TARGET_FILE:ApexSenseBridgeApex6Experiment>")
            endif()
            add_test(NAME apex6_experiment_cli COMMAND powershell -NoProfile -NonInteractive
                -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_experiment_cli.ps1"
                -Executable "$<TARGET_FILE:ApexSenseBridgeApex6Experiment>")
        endif()
    endif()
endif()

if(TRUE)
    add_library(asb_apex6_live STATIC src/apex6/live/Stream.cpp src/apex6/live/Live.cpp)
    if(ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS)
        target_compile_definitions(asb_apex6_live PUBLIC ASB_APEX6_LIVE_DEBUG_NO_DISPATCH_LATENESS=1)
    endif()
    target_link_libraries(asb_apex6_live PUBLIC asb_apex6_session)
    if(WIN32)
        target_link_libraries(asb_apex6_live PUBLIC asb_capture)
    endif()
    if(WIN32 AND ASB_BUILD_APEX6_LIVE_BRIDGE)
        add_library(asb_apex6_live_windows STATIC src/apex6/experiment/WindowsIo.cpp src/apex6/experiment/WindowsSessionLock.cpp src/platform/windows/WindowsHidDiscovery.cpp)
        target_compile_definitions(asb_apex6_live_windows PUBLIC ASB_APEX6_LIVE_RUNNER=1)
        target_link_libraries(asb_apex6_live_windows PUBLIC asb_apex6_live PRIVATE hid setupapi cfgmgr32 ole32 bcrypt)
        add_executable(ApexSenseBridgeApex6LiveBridge src/apex6/live/main.cpp)
        target_include_directories(ApexSenseBridgeApex6LiveBridge PRIVATE "${CMAKE_BINARY_DIR}/generated" tests)
        target_link_libraries(ApexSenseBridgeApex6LiveBridge PRIVATE asb_apex6_live_windows asb_apex6_fake)
    elseif(NOT WIN32)
        target_sources(asb_apex6_live PRIVATE src/capture/RawCapture.cpp)
    endif()
    if(BUILD_TESTING)
        add_executable(ApexSenseBridgeApex6LiveTests tests/test_apex6_live.cpp)
        target_link_libraries(ApexSenseBridgeApex6LiveTests PRIVATE asb_apex6_live asb_apex6_fake)
        add_test(NAME apex6_live_stream COMMAND ApexSenseBridgeApex6LiveTests)
        add_executable(ApexSenseBridgeApex6IntegratedTests tests/test_apex6_integrated.cpp)
        target_link_libraries(ApexSenseBridgeApex6IntegratedTests PRIVATE asb_apex6_live asb_apex6_fake)
        add_test(NAME apex6_integrated_hour COMMAND ApexSenseBridgeApex6IntegratedTests)
        if(WIN32)
            add_executable(ApexSenseBridgeIntegratedRawTests tests/test_integrated_raw_backend.cpp)
            target_link_libraries(ApexSenseBridgeIntegratedRawTests PRIVATE asb_core asb_apex6_live)
            add_test(NAME integrated_raw_callback_lifecycle COMMAND ApexSenseBridgeIntegratedRawTests $<TARGET_FILE:CaptureFakeViiper>)
            add_executable(ApexSenseBridgeApex6ControlTests tests/test_apex6_controls.cpp)
            target_link_libraries(ApexSenseBridgeApex6ControlTests PRIVATE asb_core)
            add_test(NAME apex6_settings_and_controls COMMAND ApexSenseBridgeApex6ControlTests)
        endif()
        if(WIN32 AND ASB_BUILD_APEX6_LIVE_BRIDGE)
            add_executable(ApexSenseBridgeApex6LiveWindowsTests tests/test_apex6_live_windows.cpp)
            target_link_libraries(ApexSenseBridgeApex6LiveWindowsTests PRIVATE asb_apex6_live_windows asb_apex6_fake)
            add_test(NAME apex6_live_native COMMAND ApexSenseBridgeApex6LiveWindowsTests $<TARGET_FILE:CaptureFakeViiper>)
            add_test(NAME apex6_live_cli COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_live_cli.ps1" -Executable "$<TARGET_FILE:ApexSenseBridgeApex6LiveBridge>")
            add_test(NAME apex6_live_hidhide COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_hidhide.ps1")
            get_target_property(ASB_LIVE_LINKS ApexSenseBridgeApex6LiveBridge LINK_LIBRARIES)
            get_target_property(ASB_LIVE_CORE_LINKS asb_core LINK_LIBRARIES)
            add_test(NAME apex6_live_isolation COMMAND ${CMAKE_COMMAND}
                "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}" "-DLIVE_LINKS=${ASB_LIVE_LINKS}" "-DCORE_LINKS=${ASB_LIVE_CORE_LINKS}"
                "-DCAPTURE_LINKS=$<TARGET_PROPERTY:asb_capture,LINK_LIBRARIES>"
                -P "${CMAKE_SOURCE_DIR}/tests/check_apex6_live_isolation.cmake")
        endif()
    endif()
endif()

if(WIN32)
    add_library(asb_apex6_integrated STATIC src/apex6/experiment/WindowsIo.cpp
        src/apex6/experiment/WindowsSessionLock.cpp)
    target_compile_definitions(asb_apex6_integrated PUBLIC ASB_APEX6_LIVE_RUNNER=1 ASB_APEX6_INTEGRATED=1)
    target_link_libraries(asb_apex6_integrated PUBLIC asb_apex6_live asb_core PRIVATE hid setupapi cfgmgr32 ole32 bcrypt)
    target_link_libraries(ApexSenseBridge PRIVATE asb_apex6_integrated)
endif()

# Pure Apex 6 protocol/DSP port: deliberately absent from BOTH production core
# and capture link graphs. No OS transport or actuator executable exists here.
add_library(asb_apex6_offline STATIC src/apex6/Gpa6.cpp src/apex6/GripDsp.cpp)
target_include_directories(asb_apex6_offline PUBLIC src)
add_executable(ApexSenseBridgeHapticOffline src/apex6/offline_main.cpp)
target_link_libraries(ApexSenseBridgeHapticOffline PRIVATE asb_apex6_offline)
if(MSVC)
    target_compile_options(asb_apex6_offline PRIVATE /W4 /permissive- /EHsc /fp:strict)
    target_compile_options(ApexSenseBridgeHapticOffline PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(asb_apex6_offline PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math -ffp-contract=off)
    target_compile_options(ApexSenseBridgeHapticOffline PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(BUILD_TESTING)
    add_executable(ApexSenseBridgeApex6OfflineTests tests/test_apex6_offline.cpp)
    target_link_libraries(ApexSenseBridgeApex6OfflineTests PRIVATE asb_apex6_offline)
    add_test(NAME apex6_offline COMMAND ApexSenseBridgeApex6OfflineTests)
    if(WIN32)
        add_test(NAME apex6_offline_cli COMMAND powershell -NoProfile -NonInteractive
            -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/test_apex6_offline_cli.ps1"
            -Executable "$<TARGET_FILE:ApexSenseBridgeHapticOffline>")
    endif()
    get_target_property(APEX6_SOURCES asb_apex6_offline SOURCES)
    get_target_property(APEX6_LINKS asb_apex6_offline LINK_LIBRARIES)
    get_target_property(OFFLINE_LINKS ApexSenseBridgeHapticOffline LINK_LIBRARIES)
    get_target_property(CORE_LINKS asb_core LINK_LIBRARIES)
    add_test(NAME apex6_offline_isolation COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}" "-DAPEX6_SOURCES=${APEX6_SOURCES}" "-DAPEX6_LINKS=${APEX6_LINKS}" "-DOFFLINE_LINKS=${OFFLINE_LINKS}" "-DCORE_LINKS=${CORE_LINKS}"
        -P "${CMAKE_SOURCE_DIR}/tests/check_apex6_isolation.cmake")
endif()

# Capture has an explicit source allowlist. Never link asb_core here: that
# library includes physical vendor-output transport and startup/reset guards.
if(WIN32)
    add_library(asb_capture STATIC
        src/capture/RawCapture.cpp
        src/capture/CaptureVerifier.cpp
        src/capture/CaptureInputPolicy.cpp
        src/capture/FeedbackRecorder.cpp
        src/capture/WindowsHash.cpp
        src/capture/WindowsCaptureBackend.cpp
        src/capture/WindowsCaptureInput.cpp
        src/capture/WindowsFixturePlayer.cpp
        src/platform/windows/WindowsHidDiscovery.cpp
        src/platform/windows/WindowsGenericHidInputSource.cpp
        src/platform/XInputMapping.cpp
        src/dualsense/DualSenseInput.cpp
        src/platform/AudioEndpointProtection.cpp
        src/platform/windows/WindowsAudioEndpointProtection.cpp
    )
    target_include_directories(asb_capture PUBLIC src)
    target_link_libraries(asb_capture PRIVATE bcrypt ole32 uuid hid setupapi cfgmgr32 xinput9_1_0)
    add_executable(ApexSenseBridgeCapture src/capture/main.cpp ${ASB_RC_FILE})
    target_compile_definitions(ApexSenseBridgeCapture PRIVATE
        ASB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ASB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ASB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        ASB_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(ApexSenseBridgeCapture PRIVATE asb_capture)
    if(BUILD_TESTING)
        get_target_property(ASB_CAPTURE_SOURCES asb_capture SOURCES)
        get_target_property(ASB_CAPTURE_LINKS asb_capture LINK_LIBRARIES)
        add_test(NAME capture_output_isolation COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}" "-DCAPTURE_SOURCES=${ASB_CAPTURE_SOURCES}" "-DCAPTURE_LINKS=${ASB_CAPTURE_LINKS}"
            -P "${CMAKE_SOURCE_DIR}/tests/check_capture_isolation.cmake")
        add_executable(ApexSenseBridgeCaptureTests tests/test_capture.cpp)
        target_link_libraries(ApexSenseBridgeCaptureTests PRIVATE asb_capture)
        add_test(NAME raw_capture COMMAND ApexSenseBridgeCaptureTests)
        add_library(CaptureFakeViiper SHARED tests/capture_fake_viiper.cpp)
        add_executable(ApexSenseBridgeCaptureBackendTests tests/test_capture_backend.cpp)
        target_link_libraries(ApexSenseBridgeCaptureBackendTests PRIVATE asb_capture)
        add_test(NAME capture_backend_lifecycle COMMAND ApexSenseBridgeCaptureBackendTests $<TARGET_FILE:CaptureFakeViiper>)
        add_executable(ApexSenseBridgeCaptureAbiTests tests/test_capture_real_abi.cpp)
        target_link_libraries(ApexSenseBridgeCaptureAbiTests PRIVATE asb_capture)
        add_test(NAME capture_real_dll_abi COMMAND ApexSenseBridgeCaptureAbiTests "$<TARGET_FILE_DIR:ApexSenseBridgeCapture>/libVIIPER.dll")
        set_tests_properties(capture_real_dll_abi PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(MSVC)
        target_compile_options(asb_capture PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_compile_options(ApexSenseBridgeCapture PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
endif()
if(BUILD_TESTING)
    if(ASB_BUILD_APEX6_EXPERIMENTS AND WIN32)
        get_target_property(ASB_EXPERIMENT_LINKS ApexSenseBridgeApex6Experiment LINK_LIBRARIES)
        get_target_property(ASB_QUERY_SOURCES asb_apex6_query_windows SOURCES)
        get_target_property(ASB_QUERY_DEFINITIONS asb_apex6_query_windows COMPILE_DEFINITIONS)
        get_target_property(ASB_QUERY_LINKS asb_apex6_query_windows LINK_LIBRARIES)
        if(TARGET ApexSenseBridgeApex6NeutralExperiment)
            get_target_property(ASB_NEUTRAL_LINKS ApexSenseBridgeApex6NeutralExperiment LINK_LIBRARIES)
        endif()
        get_target_property(ASB_CORE_LINKS asb_core LINK_LIBRARIES)
        get_target_property(ASB_RECORDER_LINKS asb_capture LINK_LIBRARIES)
        add_test(NAME apex6_experiment_isolation COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}" "-DCORE_LINKS=${ASB_CORE_LINKS}"
            "-DCAPTURE_LINKS=${ASB_RECORDER_LINKS}" "-DEXPERIMENT_LINKS=${ASB_EXPERIMENT_LINKS}" "-DQUERY_SOURCES=${ASB_QUERY_SOURCES}"
            "-DQUERY_DEFINITIONS=${ASB_QUERY_DEFINITIONS}" "-DQUERY_LINKS=${ASB_QUERY_LINKS}" "-DNEUTRAL_LINKS=${ASB_NEUTRAL_LINKS}"
            -P "${CMAKE_SOURCE_DIR}/tests/check_apex6_experiment_isolation.cmake")
    endif()
    add_executable(ApexSenseBridgeTests
        tests/test_protocol.cpp
        src/flydigi/Apex4Input.cpp
        src/flydigi/Apex4Protocol.cpp
        src/flydigi/Apex5Protocol.cpp
        src/platform/XInputMapping.cpp
    )
    target_include_directories(ApexSenseBridgeTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME protocol COMMAND ApexSenseBridgeTests)

    add_executable(ApexSenseBridgeDiagnosticTests
        tests/test_hid_diagnostics.cpp
        src/diagnostics/HidDiagnostics.cpp
    )
    target_include_directories(ApexSenseBridgeDiagnosticTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeDiagnosticTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeDiagnosticTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME hid_diagnostics COMMAND ApexSenseBridgeDiagnosticTests)

    add_executable(ApexSenseBridgeIdentityTests tests/test_identity.cpp)
    target_link_libraries(ApexSenseBridgeIdentityTests PRIVATE asb_core)
    if(MSVC)
        target_compile_options(ApexSenseBridgeIdentityTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeIdentityTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME identity_guard COMMAND ApexSenseBridgeIdentityTests)

    add_executable(ApexSenseBridgeVirtualDualSenseTests
        tests/test_virtual_dualsense.cpp
        src/dualsense/DualSenseFeedback.cpp
        src/dualsense/DualSenseFirmware.cpp
        src/dualsense/DualSenseInput.cpp
        src/dualsense/ViiperProtocol.cpp
    )
    target_include_directories(ApexSenseBridgeVirtualDualSenseTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeVirtualDualSenseTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeVirtualDualSenseTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME virtual_dualsense_protocol COMMAND ApexSenseBridgeVirtualDualSenseTests)

    add_executable(ApexSenseBridgeTriggerTranslationTests
        tests/test_adaptive_trigger_translation.cpp
        src/dualsense/AdaptiveTriggerTranslation.cpp
    )
    target_include_directories(ApexSenseBridgeTriggerTranslationTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeTriggerTranslationTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeTriggerTranslationTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME adaptive_trigger_translation COMMAND ApexSenseBridgeTriggerTranslationTests)

    add_executable(ApexSenseBridgeRumbleTests tests/test_rumble_bridge.cpp)
    target_link_libraries(ApexSenseBridgeRumbleTests PRIVATE asb_core)
    if(MSVC)
        target_compile_options(ApexSenseBridgeRumbleTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeRumbleTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME rumble_bridge COMMAND ApexSenseBridgeRumbleTests)

    add_executable(ApexSenseBridgeHapticProcessorTests
        tests/test_haptic_processor.cpp
        src/haptics/HapticProcessor.cpp
    )
    target_include_directories(ApexSenseBridgeHapticProcessorTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeHapticProcessorTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeHapticProcessorTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME haptic_processor COMMAND ApexSenseBridgeHapticProcessorTests)

    add_executable(ApexSenseBridgeXInputMappingTests
        tests/test_xinput_mapping.cpp
        src/platform/XInputMapping.cpp
    )
    target_include_directories(ApexSenseBridgeXInputMappingTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeXInputMappingTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeXInputMappingTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME xinput_mapping COMMAND ApexSenseBridgeXInputMappingTests)

    add_executable(ApexSenseBridgeViewTouchpadGestureTests
        tests/test_view_touchpad_gesture.cpp
        src/dualsense/ViewTouchpadGesture.cpp
    )
    target_include_directories(ApexSenseBridgeViewTouchpadGestureTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeViewTouchpadGestureTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeViewTouchpadGestureTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME view_touchpad_gesture COMMAND ApexSenseBridgeViewTouchpadGestureTests)

    add_executable(ApexSenseBridgeTouchpadGestureProfileTests
        tests/test_touchpad_gesture_profile.cpp
        src/dualsense/TouchpadGestureProfile.cpp
    )
    target_include_directories(ApexSenseBridgeTouchpadGestureProfileTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeTouchpadGestureProfileTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeTouchpadGestureProfileTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME touchpad_gesture_profiles COMMAND ApexSenseBridgeTouchpadGestureProfileTests)

    add_executable(ApexSenseBridgeAudioEndpointProtectionTests
        tests/test_audio_endpoint_protection.cpp
        src/platform/AudioEndpointProtection.cpp
    )
    target_include_directories(ApexSenseBridgeAudioEndpointProtectionTests PRIVATE src)
    if(MSVC)
        target_compile_options(ApexSenseBridgeAudioEndpointProtectionTests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(ApexSenseBridgeAudioEndpointProtectionTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME audio_endpoint_identity_guard
             COMMAND ApexSenseBridgeAudioEndpointProtectionTests)

    if(WIN32)
        add_executable(ApexSenseBridgeSessionControlWindowsTests
            tests/test_session_control.cpp
        )
        target_link_libraries(ApexSenseBridgeSessionControlWindowsTests PRIVATE asb_core)
        if(MSVC)
            target_compile_options(ApexSenseBridgeSessionControlWindowsTests PRIVATE /W4 /permissive- /EHsc)
        else()
            target_compile_options(ApexSenseBridgeSessionControlWindowsTests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME session_control_windows
                 COMMAND ApexSenseBridgeSessionControlWindowsTests)

        add_executable(ApexSenseBridgeVirtualDualSenseWindowsTests
            tests/test_windows_virtual_dualsense.cpp
        )
        target_link_libraries(ApexSenseBridgeVirtualDualSenseWindowsTests PRIVATE asb_core ws2_32)
        if(MSVC)
            target_compile_options(ApexSenseBridgeVirtualDualSenseWindowsTests PRIVATE /W4 /permissive- /EHsc)
        else()
            target_compile_options(ApexSenseBridgeVirtualDualSenseWindowsTests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME virtual_dualsense_windows_lifecycle
                 COMMAND ApexSenseBridgeVirtualDualSenseWindowsTests)

        add_executable(ApexSenseBridgePhysicalIsolationWindowsTests
            tests/test_physical_controller_isolation.cpp
        )
        target_link_libraries(ApexSenseBridgePhysicalIsolationWindowsTests PRIVATE asb_core)
        if(MSVC)
            target_compile_options(ApexSenseBridgePhysicalIsolationWindowsTests PRIVATE /W4 /permissive- /EHsc)
        else()
            target_compile_options(ApexSenseBridgePhysicalIsolationWindowsTests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME physical_isolation_space_station_guard
                 COMMAND ApexSenseBridgePhysicalIsolationWindowsTests)

        add_test(NAME installer_registry_contract
                 COMMAND powershell -NoProfile -NonInteractive
                         -ExecutionPolicy Bypass
                         -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installer_registry_contract.ps1)
        add_test(NAME release_version_contract
                 COMMAND powershell -NoProfile -NonInteractive
                         -ExecutionPolicy Bypass
                         -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify-version-consistency.ps1)
    endif()
endif()
