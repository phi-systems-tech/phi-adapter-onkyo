cmake_minimum_required(VERSION 3.21)

project(phi-adapter-onkyo
    VERSION 1.0.0
    DESCRIPTION "Phi Onkyo/Pioneer adapter plugin"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

option(PHI_ADAPTER_ONKYO_BUILD_IPC
    "Build IPC sidecar executable for onkyo adapter"
    ON
)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout. If not found, find_package(phi-adapter-sdk) is used."
)
set(PHI_ADAPTER_SDK_QT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk-qt" CACHE PATH
    "Path to local phi-adapter-sdk-qt checkout. If not found, find_package(phi-adapter-sdk-qt) is used."
)
option(PHI_ADAPTER_ONKYO_USE_LOCAL_ADAPTER_SDK
    "Use local ../phi-adapter-sdk checkout when available"
    ON
)

if(PHI_ADAPTER_ONKYO_BUILD_IPC)
    if(NOT TARGET phi::adapter-sdk)
        if(PHI_ADAPTER_ONKYO_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk from: ${PHI_ADAPTER_SDK_SOURCE_DIR}")
            add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
        else()
            message(STATUS "Using installed phi-adapter-sdk via find_package")
            find_package(phi-adapter-sdk CONFIG REQUIRED)
        endif()
    endif()

    if(NOT TARGET phi::adapter-sdk)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk not found")
    endif()
    if(NOT TARGET phi::adapter-sdk-qt)
        if(PHI_ADAPTER_ONKYO_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_QT_SOURCE_DIR}/CMakeLists.txt")
            message(STATUS "Using local phi-adapter-sdk-qt from: ${PHI_ADAPTER_SDK_QT_SOURCE_DIR}")
            add_subdirectory("${PHI_ADAPTER_SDK_QT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk-qt")
        else()
            message(STATUS "Using installed phi-adapter-sdk-qt via find_package")
            find_package(phi-adapter-sdk-qt CONFIG REQUIRED)
        endif()
    endif()
    if(NOT TARGET phi::adapter-sdk-qt)
        message(FATAL_ERROR "phi-adapter-sdk target phi::adapter-sdk-qt not found")
    endif()

    add_executable(phi_adapter_onkyo_ipc
        src/onkyoipcmain.cpp
    )

    target_compile_features(phi_adapter_onkyo_ipc PRIVATE cxx_std_20)

    target_link_libraries(phi_adapter_onkyo_ipc
        PRIVATE
            Qt6::Core
            Qt6::Network
            phi::adapter-sdk
            phi::adapter-sdk-qt
    )

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
