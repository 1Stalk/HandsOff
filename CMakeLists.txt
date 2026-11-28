cmake_minimum_required(VERSION 3.20)
project(HandsOff VERSION 1.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
option(HANDSOFF_WERROR "Treat compiler warnings as errors" OFF)

if(MSVC)
    string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/O[0-9a-z]+" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    add_compile_options(/W4 /MP /utf-8)
    if(HANDSOFF_WERROR)
        add_compile_options(/WX)
    endif()
endif()

set(DLL_WIN_LIBS rstrtmgr version advapi32 user32 gdi32 shell32 ole32 oleaut32 uxtheme dwmapi oleacc)
set(CTL_WIN_LIBS advapi32 user32 gdi32 shell32 dwmapi)

function(apply_size_optimized_flags target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/O1 /Os /Ob1 /Oi /Gw /Gy /GL /GR- /EHs-c->
            $<$<CONFIG:Debug>:/O2 /EHsc>
        )
        target_compile_definitions(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:_HAS_EXCEPTIONS=0>
        )
        target_link_options(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/LTCG /OPT:REF /OPT:ICF=5 /EMITPOGOPHASEINFO /NOCOFFGRPINFO /NODEFAULTLIB:libucrt.lib>
        )
        target_link_libraries(${target_name} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:ucrt.lib>
        )
    endif()
endfunction()

add_library(HandsOffDll SHARED
    src/lock_finder.cpp
    src/dllmain.cpp
    src/file_in_use_hook.cpp
    src/dialog_enhancer.cpp
    src/min_runtime.cpp
    src/hands_off.def
)
set_target_properties(HandsOffDll PROPERTIES OUTPUT_NAME "HandsOff" PREFIX "")
target_link_libraries(HandsOffDll PRIVATE ${DLL_WIN_LIBS})
apply_size_optimized_flags(HandsOffDll)

add_executable(HandsOffCtl WIN32
    src/main.cpp
    src/gui.cpp
    src/min_runtime.cpp
)
set_target_properties(HandsOffCtl PROPERTIES OUTPUT_NAME "HandsOff")
target_link_libraries(HandsOffCtl PRIVATE ${CTL_WIN_LIBS})
apply_size_optimized_flags(HandsOffCtl)

include(CTest)
if(BUILD_TESTING)
    add_executable(HandsOffTests
        src/lock_finder.cpp
        src/dialog_enhancer.cpp
        src/file_in_use_hook.cpp
        tests/handsoff_tests.cpp
    )
    set_target_properties(HandsOffTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    if(MSVC)
        target_compile_options(HandsOffTests PRIVATE /EHsc $<$<NOT:$<CONFIG:Debug>>:/O2 /Ob2>)
    endif()
    target_link_libraries(HandsOffTests PRIVATE ${DLL_WIN_LIBS} uuid)

    add_test(NAME HandsOffUnitAndLockTests COMMAND HandsOffTests)
    add_test(NAME HandsOffCliScanReadme COMMAND HandsOffCtl scan "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
endif()
