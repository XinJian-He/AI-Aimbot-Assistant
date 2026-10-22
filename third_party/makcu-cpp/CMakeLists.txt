cmake_minimum_required(VERSION 3.20)
project(makcu-cpp VERSION 1.3.5 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Link-Time Optimization for Release builds
include(CheckIPOSupported)
check_ipo_supported(RESULT MAKCU_IPO_SUPPORTED OUTPUT MAKCU_IPO_OUTPUT)

# Build type options
option(BUILD_SHARED_ONLY "Build only shared library" OFF)
option(BUILD_STATIC_ONLY "Build only static library" OFF)
option(MAKCU_ENABLE_NATIVE_OPTIMIZATIONS "Enable CPU-specific optimizations (-march=native)" OFF)

# Compiler-specific optimizations
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
    if(MAKCU_ENABLE_NATIVE_OPTIMIZATIONS)
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")
    endif()
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /Wall")
endif()

# Find required packages
find_package(Threads REQUIRED)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    # Linux-specific libraries
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UDEV REQUIRED libudev)
endif()

# Library source files
set(LIBRARY_SOURCES
    makcu-cpp/src/makcu.cpp
    makcu-cpp/src/serialport.cpp
    makcu-cpp/src/makcu_c.cpp
)

# Public headers (for distribution)
set(PUBLIC_HEADERS
    makcu-cpp/include/makcu.h
    makcu-cpp/include/makcu_c.h
)

# All headers (including internal ones for build)
set(HEADERS
    ${PUBLIC_HEADERS}
    makcu-cpp/include/serialport.h
)

# Create both shared and static libraries by default
set(LIBRARY_TARGETS)

# Build shared library (unless BUILD_STATIC_ONLY is set)
if(NOT BUILD_STATIC_ONLY)
    add_library(${PROJECT_NAME}_shared SHARED ${LIBRARY_SOURCES} ${HEADERS})
    set_target_properties(${PROJECT_NAME}_shared PROPERTIES
        OUTPUT_NAME ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        PUBLIC_HEADER "${PUBLIC_HEADERS}"
    )
    if(WIN32)
        # Avoid .lib name collision with the static library on Windows.
        set_target_properties(${PROJECT_NAME}_shared PROPERTIES
            ARCHIVE_OUTPUT_NAME "${PROJECT_NAME}_import"
        )
    endif()
    list(APPEND LIBRARY_TARGETS ${PROJECT_NAME}_shared)
endif()

# Build static library (unless BUILD_SHARED_ONLY is set)
if(NOT BUILD_SHARED_ONLY)
    add_library(${PROJECT_NAME}_static STATIC ${LIBRARY_SOURCES} ${HEADERS})
    set_target_properties(${PROJECT_NAME}_static PROPERTIES
        OUTPUT_NAME ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        PUBLIC_HEADER "${PUBLIC_HEADERS}"
    )
    list(APPEND LIBRARY_TARGETS ${PROJECT_NAME}_static)
endif()

# Create alias for backward compatibility - prefer shared library
if(NOT BUILD_STATIC_ONLY)
    add_library(${PROJECT_NAME} ALIAS ${PROJECT_NAME}_shared)
else()
    add_library(${PROJECT_NAME} ALIAS ${PROJECT_NAME}_static)
endif()

# Configure all library targets
foreach(target ${LIBRARY_TARGETS})
    # Include directories for the library
    target_include_directories(${target} 
        PUBLIC 
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/makcu-cpp/include>
            $<INSTALL_INTERFACE:include/makcu>
    )

    # Require C++23 via target_compile_features
    target_compile_features(${target} PUBLIC cxx_std_23)

    # Enable LTO for Release builds
    if(MAKCU_IPO_SUPPORTED)
        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
        )
    endif()

    # Link libraries
    target_link_libraries(${target} PUBLIC Threads::Threads)

    # Platform-specific linking
    if(WIN32)
        target_link_libraries(${target} PUBLIC setupapi)
        target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0601)
        
        # Export symbols for shared library on Windows
        if(${target} STREQUAL "${PROJECT_NAME}_shared")
            target_compile_definitions(${target} PRIVATE MAKCU_EXPORTS)
            target_compile_definitions(${target} INTERFACE MAKCU_SHARED)
        endif()
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(${target} PUBLIC ${UDEV_LIBRARIES})
        target_include_directories(${target} PUBLIC ${UDEV_INCLUDE_DIRS})
        target_compile_options(${target} PUBLIC ${UDEV_CFLAGS_OTHER})
    endif()

    # Set output directory
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endforeach()

# Install targets
install(TARGETS ${LIBRARY_TARGETS}
    EXPORT makcu-cppTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include/makcu
)


# Create and install CMake config files
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/makcu-cppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

export(EXPORT makcu-cppTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/makcu-cppTargets.cmake"
    NAMESPACE makcu::
)

# Generate CMake config file inline
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/makcu-cppConfig.cmake" "
include(CMakeFindDependencyMacro)

# Find required dependencies
find_dependency(Threads REQUIRED)

# Platform-specific dependencies
if(UNIX AND NOT APPLE)
    find_dependency(PkgConfig REQUIRED)
    pkg_check_modules(UDEV REQUIRED libudev)
endif()

# Include the targets file
include(\"\${CMAKE_CURRENT_LIST_DIR}/makcu-cppTargets.cmake\")

# Create convenient alias - prefer shared library if available, fallback to static
if(TARGET makcu::makcu-cpp_shared)
    if(NOT TARGET makcu::makcu-cpp)
        add_library(makcu::makcu-cpp ALIAS makcu::makcu-cpp_shared)
    endif()
elseif(TARGET makcu::makcu-cpp_static)
    if(NOT TARGET makcu::makcu-cpp)
        add_library(makcu::makcu-cpp ALIAS makcu::makcu-cpp_static)
    endif()
endif()
")

install(EXPORT makcu-cppTargets
    FILE makcu-cppTargets.cmake
    NAMESPACE makcu::
    DESTINATION lib/cmake/makcu-cpp
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/makcu-cppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/makcu-cppConfigVersion.cmake"
    DESTINATION lib/cmake/makcu-cpp
)

# Package configuration
set(CPACK_PACKAGE_NAME "makcu-cpp")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-Performance C++ Library for MAKCU Mouse Controllers")
set(CPACK_PACKAGE_VENDOR "K4HVH")
set(CPACK_PACKAGE_CONTACT "https://github.com/K4HVH/makcu-cpp")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/K4HVH/makcu-cpp")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# Platform-specific package configuration
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    
    # NSIS Configuration for versioned subfolder structure
    set(CPACK_NSIS_DISPLAY_NAME "MAKCU C++ Library")
    set(CPACK_NSIS_PACKAGE_NAME "MAKCU C++ Library")
    set(CPACK_NSIS_CONTACT "https://github.com/K4HVH/makcu-cpp")
    set(CPACK_NSIS_HELP_LINK "https://github.com/K4HVH/makcu-cpp")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/K4HVH/makcu-cpp")
    set(CPACK_NSIS_MODIFY_PATH ON)
    
    # Key setting: Configure install directory structure
    # This creates: C:\Program Files\makcu-cpp\1.1.0
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "makcu-cpp\\\\${PROJECT_VERSION}")
    
    # Optional: Set a more user-friendly program name in Add/Remove Programs
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\makcu-cpp.exe")
    set(CPACK_NSIS_DISPLAY_NAME "MAKCU C++ Library ${PROJECT_VERSION}")
    
    # Create start menu shortcuts (optional)
    set(CPACK_NSIS_MENU_LINKS
        "https://github.com/K4HVH/makcu-cpp" "MAKCU C++ Library Homepage"
    )
    
    # Allow users to choose installation directory
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    
else()
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libudev1")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "K4HVH")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/K4HVH/makcu-cpp")
    set(CPACK_RPM_PACKAGE_REQUIRES "systemd-devel")
    set(CPACK_RPM_PACKAGE_URL "https://github.com/K4HVH/makcu-cpp")
endif()

include(CPack)
