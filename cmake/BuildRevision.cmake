# Keep --version tied to the source used by each build, including rebuilds
# after a commit. Do not touch the header when its contents have not changed.
execute_process(COMMAND git describe --always --dirty --abbrev=12
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT revision)
    set(revision "unknown")
endif()
string(REPLACE "\\" "\\\\" revision "${revision}")
string(REPLACE "\"" "\\\"" revision "${revision}")
set(content "#define TERMICA_BUILD_REVISION \"${revision}\"\n")
if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${OUTPUT_FILE}" "${content}")
endif()
