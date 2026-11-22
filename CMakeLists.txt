cmake_minimum_required(VERSION 3.20)
project(HandsOff VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
option(HANDSOFF_WERROR "Treat compiler warnings as errors" OFF)

if(MSVC)
    add_compile_options(/W4 /EHsc /MP /utf-8)
    if(HANDSOFF_WERROR)
        add_compile_options(/WX)
    endif()
endif()

set(WIN_LIBS rstrtmgr version advapi32 user32 gdi32 shell32 ole32 oleaut32 uuid uxtheme dwmapi)

add_library(HandsOffCore OBJECT src/lock_finder.cpp)

add_library(HandsOffDll SHARED
    $<TARGET_OBJECTS:HandsOffCore>
    src/dllmain.cpp
    src/file_in_use_hook.cpp
    src/dialog_enhancer.cpp
    src/hands_off.def
)
set_target_properties(HandsOffDll PROPERTIES OUTPUT_NAME "HandsOff" PREFIX "")
target_link_libraries(HandsOffDll PRIVATE ${WIN_LIBS} oleacc)

add_executable(HandsOffCtl WIN32
    $<TARGET_OBJECTS:HandsOffCore>
    src/main.cpp
    src/gui.cpp
)
set_target_properties(HandsOffCtl PROPERTIES OUTPUT_NAME "HandsOff")
target_link_libraries(HandsOffCtl PRIVATE ${WIN_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_executable(HandsOffTests
        $<TARGET_OBJECTS:HandsOffCore>
        src/dialog_enhancer.cpp
        src/file_in_use_hook.cpp
        tests/handsoff_tests.cpp
    )
    set_target_properties(HandsOffTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    target_link_libraries(HandsOffTests PRIVATE ${WIN_LIBS} oleacc)

    add_test(NAME HandsOffUnitAndLockTests COMMAND HandsOffTests)
    add_test(NAME HandsOffCliScanReadme COMMAND HandsOffCtl scan "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
endif()
