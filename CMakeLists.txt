# CMakeList.txt: DomDownTool 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.24)

option(IS_AUTODOWNLOAD_VCPKG "是否自动下载vcpkg，并配置vcpkg为工具链" ON)
option(DOMDOWNTOOL_BUILD_UI "是否构建 WebView UI 预览界面" ON)

message(设置代理：127.0.0.1:10090)
set(ENV{https_proxy} "127.0.0.1:10090")
set(ENV{http_proxy} "127.0.0.1:10090")

IF(IS_AUTODOWNLOAD_VCPKG)
    set(Vcpkg_DownloadPath ${CMAKE_CURRENT_SOURCE_DIR}/../out/vcpkg)
    #初始化vcpkg
    IF(IS_DIRECTORY ${Vcpkg_DownloadPath})
        IF(WIN32)
            set(VCPKG_EXECUTABLE ${Vcpkg_DownloadPath}/vcpkg.exe)
        ELSE()
            set(VCPKG_EXECUTABLE ${Vcpkg_DownloadPath}/vcpkg)
        ENDIF ()
        IF(EXISTS ${VCPKG_EXECUTABLE})
            message("vcpkg已经存在，不需要再次下载")
        ELSE()
            message("下载vcpkg中2...")
            IF(WIN32)
                execute_process(COMMAND ${Vcpkg_DownloadPath}/bootstrap-vcpkg.bat)
            ELSE()
                execute_process(COMMAND ${Vcpkg_DownloadPath}/bootstrap-vcpkg.sh)
            ENDIF()
        ENDIF()
    ELSE()
        message("下载vcpkg中...")
        execute_process(COMMAND git clone https://github.com/microsoft/vcpkg.git ${Vcpkg_DownloadPath})
        IF(WIN32)
        	execute_process(COMMAND ${Vcpkg_DownloadPath}/bootstrap-vcpkg.bat)
        ELSE()
    	    execute_process(COMMAND ${Vcpkg_DownloadPath}/bootstrap-vcpkg.sh)
        ENDIF()
    ENDIF()

    set(CMAKE_TOOLCHAIN_FILE ${Vcpkg_DownloadPath}/scripts/buildsystems/vcpkg.cmake)
ENDIF ()

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

IF(WIN32)
    # 必须在add_library ， add_executable 前设置，否则无效。让脚本的字符串使用utf8格式编译
    add_compile_options(/utf-8)
    #设置msvc的c++标准，不添加编译器会无法识别__cplusplus
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")
ENDIF()

project ("DomDownTool")

if(MSVC)
    # 混合工具链环境中，CMake 可能误用 PATH 里的 GNU ar/ld。
    get_filename_component(DOMDOWNTOOL_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(DOMDOWNTOOL_MSVC_LIBRARIAN "${DOMDOWNTOOL_MSVC_BIN_DIR}/lib.exe")
    set(DOMDOWNTOOL_MSVC_LINKER "${DOMDOWNTOOL_MSVC_BIN_DIR}/link.exe")
    if(EXISTS "${DOMDOWNTOOL_MSVC_LIBRARIAN}")
        set(CMAKE_AR "${DOMDOWNTOOL_MSVC_LIBRARIAN}")
    endif()
    if(EXISTS "${DOMDOWNTOOL_MSVC_LINKER}")
        set(CMAKE_LINKER "${DOMDOWNTOOL_MSVC_LINKER}")
    endif()
endif()

find_package(httplib CONFIG REQUIRED)
find_package(GDAL CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)

if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
    # GeographicLib 2.4 的版本脚本还未适配 VS18 的 19.5x 编译器版本号。
    set(DOMDOWNTOOL_MSVC_TOOLSET_VERSION ${MSVC_TOOLSET_VERSION})
    set(MSVC_TOOLSET_VERSION 145)
endif()
find_package(GeographicLib CONFIG REQUIRED)
if(DEFINED DOMDOWNTOOL_MSVC_TOOLSET_VERSION)
    set(MSVC_TOOLSET_VERSION ${DOMDOWNTOOL_MSVC_TOOLSET_VERSION})
endif()

find_package(fmt CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenCV REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(assimp CONFIG REQUIRED)

set(DOMDOWNTOOL_LIBRARIES
    httplib::httplib
    GDAL::GDAL
    LibXml2::LibXml2
    ${GeographicLib_LIBRARIES}
    fmt::fmt-header-only
    CURL::libcurl
    ${OpenCV_LIBS}
    OpenSSL::Crypto
    assimp::assimp)

file(GLOB DOMDOWNTOOL_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM DOMDOWNTOOL_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_main.cpp)

add_library(DomDownToolCore STATIC ${DOMDOWNTOOL_CORE_SOURCES})
target_include_directories(DomDownToolCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(DomDownToolCore PUBLIC ${DOMDOWNTOOL_LIBRARIES})

# 将源代码添加到此项目的可执行文件。
add_executable(DomDownTool ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(DomDownTool PRIVATE DomDownToolCore)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET DomDownToolCore PROPERTY CXX_STANDARD 20)
  set_property(TARGET DomDownTool PROPERTY CXX_STANDARD 20)
endif()

if(MSVC)
  target_compile_definitions(DomDownToolCore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(DomDownTool PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(DOMDOWNTOOL_BUILD_UI)
  include(FetchContent)

  set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview
    GIT_TAG 0.12.0)
  FetchContent_MakeAvailable(webview)

  if(WIN32)
    add_executable(DomDownToolUi WIN32
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_main.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/UiBridge.cpp)
  else()
    add_executable(DomDownToolUi
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_main.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/UiBridge.cpp)
  endif()

  target_link_libraries(DomDownToolUi PRIVATE webview::core DomDownToolCore)
  target_compile_definitions(DomDownToolUi PRIVATE
    DOMDOWNTOOL_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/ui")
  if (CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET DomDownToolUi PROPERTY CXX_STANDARD 20)
  endif()
  if(MSVC)
    target_compile_definitions(DomDownToolUi PRIVATE NOMINMAX)
    target_compile_options(DomDownToolUi PRIVATE /EHsc)
  endif()
endif()

enable_testing()

function(add_domdown_test target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE DomDownToolCore)
  target_compile_definitions(${target_name} PRIVATE
    DOMDOWNTOOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if (CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET ${target_name} PROPERTY CXX_STANDARD 20)
  endif()
  if(MSVC)
    target_compile_definitions(${target_name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

add_domdown_test(PostProcessPyramidTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/PostProcessPyramidTest.cpp)
add_domdown_test(PostProcessMergedImageTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/PostProcessMergedImageTest.cpp)
add_domdown_test(PostProcessFbxTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/PostProcessFbxTest.cpp)
add_domdown_test(BaiduProjectionTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/BaiduProjectionTest.cpp)
add_domdown_test(TileProviderUrlTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/TileProviderUrlTest.cpp)
add_domdown_test(TianDiTuDownloadTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/TianDiTuDownloadTest.cpp)
add_domdown_test(TianDiTuMultidateDownloadTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/TianDiTuMultidateDownloadTest.cpp)
add_domdown_test(RealDataIntegrationTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/RealDataIntegrationTest.cpp)
add_domdown_test(RealDataFbxIntegrationTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/RealDataFbxIntegrationTest.cpp)
add_domdown_test(UiBridgeSmokeTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/UiBridgeSmokeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/UiBridge.cpp)
