if(NOT DEFINED DOMDOWNTOOL_DUMPBIN OR NOT EXISTS "${DOMDOWNTOOL_DUMPBIN}")
  message(FATAL_ERROR "dumpbin executable is unavailable")
endif()

if(NOT DEFINED DOMDOWNTOOL_BINARY OR NOT EXISTS "${DOMDOWNTOOL_BINARY}")
  message(FATAL_ERROR "binary is unavailable: ${DOMDOWNTOOL_BINARY}")
endif()

execute_process(
  COMMAND "${DOMDOWNTOOL_DUMPBIN}" /dependents "${DOMDOWNTOOL_BINARY}"
  RESULT_VARIABLE dumpbin_result
  OUTPUT_VARIABLE dumpbin_output
  ERROR_VARIABLE dumpbin_error
  ENCODING UTF-8)

if(NOT dumpbin_result EQUAL 0)
  message(FATAL_ERROR
    "dumpbin failed for ${DOMDOWNTOOL_BINARY}: ${dumpbin_error}")
endif()

string(TOLOWER "${dumpbin_output}\n${dumpbin_error}" dependency_output)
set(dynamic_msvc_runtime_pattern
    "(vcruntime[0-9][a-z0-9_]*|msvcp[0-9][a-z0-9_]*|concrt[0-9][a-z0-9_]*|ucrtbased?|api-ms-win-crt-[a-z0-9-]+)\\.dll")

if(dependency_output MATCHES "${dynamic_msvc_runtime_pattern}")
  message(FATAL_ERROR
    "${DOMDOWNTOOL_BINARY} depends on dynamic MSVC runtime ${CMAKE_MATCH_0}")
endif()

message(STATUS "${DOMDOWNTOOL_BINARY} uses the static MSVC runtime")
