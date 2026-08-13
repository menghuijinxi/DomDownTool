# CMakeList.txt: DomDownTool 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.24)

option(IS_AUTODOWNLOAD_VCPKG "是否自动下载vcpkg，并配置vcpkg为工具链" ON)
option(DOMDOWNTOOL_BUILD_PREVIEW "是否构建 SkiaUI 瓦块预览工具" ON)

list(APPEND VCPKG_OVERLAY_PORTS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vcpkg-overlays")
list(REMOVE_DUPLICATES VCPKG_OVERLAY_PORTS)

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
        execute_process(COMMAND git clone https://github.com/menghuijinxi/vcpkg.git ${Vcpkg_DownloadPath})
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

function(domdowntool_configure_msvc_ninja_dependencies)
    if(NOT MSVC OR NOT CMAKE_GENERATOR MATCHES "^Ninja")
        return()
    endif()

    set(probe_directory "${CMAKE_BINARY_DIR}/CMakeFiles/DomDownToolShowIncludes")
    file(MAKE_DIRECTORY "${probe_directory}")
    file(WRITE "${probe_directory}/dependency.h" "\n")
    file(WRITE "${probe_directory}/main.c"
        "#include \"dependency.h\"\nint main(void) { return 0; }\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c main.c
        WORKING_DIRECTORY "${probe_directory}"
        OUTPUT_VARIABLE compiler_output
        ERROR_VARIABLE compiler_error
        RESULT_VARIABLE compiler_result
        ENCODING UTF-8)
    if(NOT compiler_result EQUAL 0 OR
       NOT compiler_output MATCHES
           "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
        message(FATAL_ERROR
            "Failed to detect the MSVC /showIncludes prefix: ${compiler_error}")
    endif()

    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

domdowntool_configure_msvc_ninja_dependencies()

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
find_package(Protobuf CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(DOMDOWNTOOL_LIBRARIES
    GDAL::GDAL
    LibXml2::LibXml2
    ${GeographicLib_LIBRARIES}
    fmt::fmt-header-only
    CURL::libcurl
    ${OpenCV_LIBS}
    OpenSSL::Crypto
    assimp::assimp
    protobuf::libprotobuf
    ZLIB::ZLIB)

file(GLOB DOMDOWNTOOL_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM DOMDOWNTOOL_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/amap_gis_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview_main.cpp)

add_library(DomDownToolCore STATIC ${DOMDOWNTOOL_CORE_SOURCES})
target_sources(DomDownToolCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/MapViewport.cpp)
protobuf_generate(
    TARGET DomDownToolCore
    LANGUAGE cpp
    PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/amap_vector_tile.proto
    APPEND_PATH
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(DomDownToolCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>)
target_link_libraries(DomDownToolCore PUBLIC ${DOMDOWNTOOL_LIBRARIES})

# 将源代码添加到此项目的可执行文件。
add_executable(DomDownTool ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(DomDownTool PRIVATE DomDownToolCore)

add_executable(AmapGisDownload ${CMAKE_CURRENT_SOURCE_DIR}/src/amap_gis_main.cpp)
target_link_libraries(AmapGisDownload PRIVATE DomDownToolCore)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET DomDownToolCore PROPERTY CXX_STANDARD 20)
  set_property(TARGET DomDownTool PROPERTY CXX_STANDARD 20)
  set_property(TARGET AmapGisDownload PROPERTY CXX_STANDARD 20)
endif()

if(MSVC)
  target_compile_definitions(DomDownToolCore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(DomDownTool PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(AmapGisDownload PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(DomDownTool PRIVATE /EHsc)
endif()

if(DOMDOWNTOOL_BUILD_PREVIEW)
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
endif()

if(DOMDOWNTOOL_BUILD_PREVIEW)
  if(NOT WIN32)
    message(FATAL_ERROR "DomDownToolPreview 当前仅支持 Windows")
  endif()

  find_package(SkiaUI CONFIG REQUIRED)
  add_executable(DomDownToolPreview WIN32
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/PreviewResources.rc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/AmapPlaceSearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/AmapWebViewSearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/Dx12GisRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/ExportImageComposer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/ExportTaskManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/GisOverlayRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/GisMeshBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/MapRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/PreviewController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/SearchTrace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/SkiaImageCodecs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/TileDownloadQueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/TileImageCache.cpp)
  set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/PreviewResources.rc
    APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/preview/index.html)
  target_link_libraries(DomDownToolPreview PRIVATE
    DomDownToolCore
    SkiaUI::SkuiWin32Dx12
    webview::core
    comctl32
    ole32
    shell32
    d3d12
    d3dcompiler)
  target_compile_definitions(DomDownToolPreview PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
  set_property(TARGET DomDownToolPreview PROPERTY CXX_STANDARD 23)
  set_property(TARGET DomDownToolPreview PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(DomDownToolPreview PRIVATE /EHsc /permissive-)
  endif()
endif()

enable_testing()

add_test(NAME SatelliteDownloadHelpTest COMMAND DomDownTool --help)
add_test(NAME SatelliteDownloadDryRunTest
    COMMAND DomDownTool --provider baidu --lon 116.74 --lat 39.95
        --radius 20000 --zoom 16 --merged-format jpg --dry-run)
add_test(NAME SatelliteDownloadInvalidProviderTest
    COMMAND DomDownTool --provider ocean)
add_test(NAME SatelliteDownloadInvalidMapTypeTest
    COMMAND DomDownTool --provider baidu --map-type electronic)
add_test(NAME SatelliteDownloadInvalidMergedFormatTest
    COMMAND DomDownTool --merged-format bmp)
set_tests_properties(
    SatelliteDownloadInvalidProviderTest
    SatelliteDownloadInvalidMapTypeTest
    SatelliteDownloadInvalidMergedFormatTest
    PROPERTIES WILL_FAIL TRUE)

add_test(NAME AmapGisDownloadHelpTest COMMAND AmapGisDownload --help)
add_test(NAME AmapGisDownloadInvalidTypeTest COMMAND AmapGisDownload --types ocean)
add_test(NAME AmapGisDownloadInvalidConcurrencyTest
    COMMAND AmapGisDownload --concurrency invalid)
add_test(NAME AmapGisDownloadInvalidMergedFormatTest
    COMMAND AmapGisDownload --merged-format bmp)
set_tests_properties(AmapGisDownloadInvalidTypeTest PROPERTIES WILL_FAIL TRUE)
set_tests_properties(AmapGisDownloadInvalidConcurrencyTest PROPERTIES WILL_FAIL TRUE)
set_tests_properties(AmapGisDownloadInvalidMergedFormatTest PROPERTIES WILL_FAIL TRUE)

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
add_domdown_test(AmapVectorGeoJsonTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/AmapVectorGeoJsonTest.cpp)
add_domdown_test(MapViewportTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/MapViewportTest.cpp)
add_domdown_test(AmapPlaceSearchTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/AmapPlaceSearchTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/AmapPlaceSearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/SearchTrace.cpp)

if(DOMDOWNTOOL_BUILD_PREVIEW)
  add_executable(TileDownloadQueueTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/TileDownloadQueueTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/SkiaImageCodecs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/TileDownloadQueue.cpp)
  target_link_libraries(TileDownloadQueueTest PRIVATE
    DomDownToolCore
    SkiaUI::SkuiWin32Dx12)
  target_compile_definitions(TileDownloadQueueTest PRIVATE
    DOMDOWNTOOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
  set_property(TARGET TileDownloadQueueTest PROPERTY CXX_STANDARD 23)
  set_property(TARGET TileDownloadQueueTest PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(TileDownloadQueueTest PRIVATE /EHsc /permissive-)
  endif()
  add_test(NAME TileDownloadQueueTest COMMAND TileDownloadQueueTest)

  add_executable(GisOverlayRendererTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/GisOverlayRendererTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/Dx12GisRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/GisOverlayRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/GisMeshBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/SearchTrace.cpp)
  target_link_libraries(GisOverlayRendererTest PRIVATE
    DomDownToolCore
    SkiaUI::SkuiWin32Dx12
    d3d12
    d3dcompiler)
  target_compile_definitions(GisOverlayRendererTest PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
  set_property(TARGET GisOverlayRendererTest PROPERTY CXX_STANDARD 23)
  set_property(TARGET GisOverlayRendererTest PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(GisOverlayRendererTest PRIVATE /EHsc /permissive-)
  endif()
  add_test(NAME GisOverlayRendererTest COMMAND GisOverlayRendererTest)

  add_executable(GisMeshBuilderTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/GisMeshBuilderTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/GisMeshBuilder.cpp)
  target_link_libraries(GisMeshBuilderTest PRIVATE DomDownToolCore)
  target_compile_definitions(GisMeshBuilderTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_property(TARGET GisMeshBuilderTest PROPERTY CXX_STANDARD 23)
  set_property(TARGET GisMeshBuilderTest PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(GisMeshBuilderTest PRIVATE /EHsc /permissive-)
  endif()
  add_test(NAME GisMeshBuilderTest COMMAND GisMeshBuilderTest)

  add_executable(PreviewSearchUiTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/PreviewSearchUiTest.cpp)
  target_link_libraries(PreviewSearchUiTest PRIVATE
    DomDownToolCore
    SkiaUI::SkuiWin32Dx12)
  target_compile_definitions(PreviewSearchUiTest PRIVATE
    DOMDOWNTOOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
  set_property(TARGET PreviewSearchUiTest PROPERTY CXX_STANDARD 23)
  set_property(TARGET PreviewSearchUiTest PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(PreviewSearchUiTest PRIVATE /EHsc /permissive-)
  endif()
  add_test(NAME PreviewSearchUiTest COMMAND PreviewSearchUiTest)

  add_executable(ExportTaskManagerTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ExportTaskManagerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/ExportImageComposer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/preview/ExportTaskManager.cpp)
  target_link_libraries(ExportTaskManagerTest PRIVATE DomDownToolCore)
  target_compile_definitions(ExportTaskManagerTest PRIVATE
    DOMDOWNTOOL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
  set_property(TARGET ExportTaskManagerTest PROPERTY CXX_STANDARD 23)
  set_property(TARGET ExportTaskManagerTest PROPERTY CXX_STANDARD_REQUIRED ON)
  if(MSVC)
    target_compile_options(ExportTaskManagerTest PRIVATE /EHsc /permissive-)
  endif()
  add_test(NAME ExportTaskManagerTest COMMAND ExportTaskManagerTest)
endif()
add_domdown_test(AmapVectorPngTileTest
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/AmapVectorPngTileTest.cpp)
