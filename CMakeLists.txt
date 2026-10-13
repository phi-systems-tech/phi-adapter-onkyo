cmake_minimum_required(VERSION 3.21)

project(phi-adapter-onkyo
    VERSION 1.0.0
    DESCRIPTION "Phi Onkyo/Pioneer adapter plugin"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No Qt. The event loop, the timers and the strings come from phi-runtime
# (through the SDK), and JSON from nlohmann, which is what phi-core uses.
find_package(nlohmann_json 3.11 REQUIRED)

option(PHI_ADAPTER_ONKYO_BUILD_IPC
    "Build IPC sidecar executable for onkyo adapter"
    ON
)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
option(PHI_ADAPTER_ONKYO_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available"
    ON
)

if(PHI_ADAPTER_ONKYO_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_ONKYO_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()

    # The receiver, split by what each part is about: the wire, the selector
    # vocabulary, addressing, the connection, the queue, the state, the device
    # descriptor, and the lifecycle that ties them together.
    add_library(phi_adapter_onkyo_core STATIC
        src/onkyo_devicemodel.cpp
        src/onkyo_endpoint.cpp
        src/onkyo_frames.cpp
        src/onkyo_inputs.cpp
        src/onkyo_instance.cpp
        src/onkyo_iscp_client.cpp
        src/onkyo_json.cpp
        src/onkyo_media.cpp
        src/onkyo_probe.cpp
        src/onkyo_queue.cpp
        src/onkyo_schema.cpp
        src/onkyo_selector.cpp
        src/onkyo_state.cpp
        src/onkyo_trace.cpp
    )
    target_compile_features(phi_adapter_onkyo_core PUBLIC cxx_std_20)
    set_target_properties(phi_adapter_onkyo_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(phi_adapter_onkyo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(phi_adapter_onkyo_core
        PUBLIC
            phi::adapter-sdk
            nlohmann_json::nlohmann_json
    )

    add_executable(phi_adapter_onkyo_ipc src/main.cpp)
    target_link_libraries(phi_adapter_onkyo_ipc PRIVATE phi_adapter_onkyo_core)

    enable_testing()

    # The eISCP wire and the selector vocabulary, tested without a receiver.
    add_executable(onkyo_wire_tests tests/onkyo_wire_tests.cpp)
    target_link_libraries(onkyo_wire_tests PRIVATE phi_adapter_onkyo_core)
    add_test(NAME onkyo_wire_tests COMMAND onkyo_wire_tests)

    # Reporting, labels and ordering: the decisions, without the lifecycle.
    add_executable(onkyo_behaviour_tests tests/onkyo_behaviour_tests.cpp)
    target_link_libraries(onkyo_behaviour_tests PRIVATE phi_adapter_onkyo_core)
    add_test(NAME onkyo_behaviour_tests COMMAND onkyo_behaviour_tests)

    # The connection, against a stub receiver on a loopback port.
    add_executable(onkyo_iscp_client_tests tests/onkyo_iscp_client_tests.cpp)
    target_link_libraries(onkyo_iscp_client_tests PRIVATE phi_adapter_onkyo_core)
    add_test(NAME onkyo_iscp_client_tests COMMAND onkyo_iscp_client_tests)

    set_tests_properties(onkyo_wire_tests onkyo_behaviour_tests onkyo_iscp_client_tests
        PROPERTIES TIMEOUT 120)

    set_target_properties(phi_adapter_onkyo_ipc PROPERTIES
        OUTPUT_NAME "phi_adapter_onkyo_ipc"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters"
    )

    install(TARGETS phi_adapter_onkyo_ipc
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
    )

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/onkyo-config.json"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters
        RENAME onkyo-config.json
    )
endif()
