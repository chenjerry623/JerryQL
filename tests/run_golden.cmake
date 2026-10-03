# Runs one SQL script through the shell and diffs it against expected output.
# Regenerate an expected file with:  ./build/jerryql --echo tests/sql/x.sql > tests/sql/x.expected
execute_process(
  COMMAND ${JERRYQL} --echo ${SQL}
  OUTPUT_VARIABLE actual
  ERROR_VARIABLE errors)
file(READ ${EXPECTED} expected)
if(NOT actual STREQUAL expected)
  file(WRITE ${EXPECTED}.actual "${actual}")
  message(FATAL_ERROR "Output differs from ${EXPECTED}.\n"
                      "Actual output written to ${EXPECTED}.actual\n${errors}")
endif()
