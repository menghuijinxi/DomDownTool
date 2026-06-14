# CMakeList.txt: DomDownTool 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.24)

option(IS_AUTODOWNLOAD_VCPKG "是否自动下载vcpkg，并配置vcpkg为工具链" ON)

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

# 将源代码添加到此项目的可执行文件。
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/src DIR_SRCS)
add_executable (DomDownTool ${DIR_SRCS})
target_include_directories(DomDownTool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(DomDownTool PRIVATE
    httplib::httplib
    GDAL::GDAL
    LibXml2::LibXml2
    ${GeographicLib_LIBRARIES}
    fmt::fmt-header-only
    CURL::libcurl
    ${OpenCV_LIBS}
    OpenSSL::Crypto)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET DomDownTool PROPERTY CXX_STANDARD 20)
endif()

if(MSVC)
  target_compile_definitions(DomDownTool PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# TODO: 如有需要，请添加测试并安装目标。
