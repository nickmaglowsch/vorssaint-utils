# Generate the recorded evdev stream, replay it through the relay, and compare
# the emitted events with tests/replay_expected.txt.
execute_process(COMMAND ${GEN} ${CMAKE_CURRENT_BINARY_DIR}/replay_stream.bin RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen-stream failed: ${rc}")
endif()

execute_process(
  COMMAND ${RELAY} --backend fake --replay ${CMAKE_CURRENT_BINARY_DIR}/replay_stream.bin
          --rules-file ${RULES} -v
  OUTPUT_VARIABLE out
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "replay failed: ${rc}")
endif()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/replay_actual.txt "${out}")
file(READ ${EXPECTED} want)
if(NOT out STREQUAL want)
  message(FATAL_ERROR
    "replay output differs from ${EXPECTED}\n"
    "--- actual ---\n${out}")
endif()
message(STATUS "replay output matches ${EXPECTED}")
