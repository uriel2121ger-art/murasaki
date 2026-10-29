# Usage: cmake -DTS=<murasaki_es.ts> [-DLUPDATE=<lupdate> -DSRC=<src/app> -DWORK=<scratch dir>] -P check_translations.cmake
# Fails if the translation file is missing, has unfinished/obsolete or empty entries, or (with lupdate) is out of date
# with the sources, i.e. a UI string was added or changed without updating and translating the file.
if(NOT TS OR NOT EXISTS "${TS}")
  message(FATAL_ERROR "translation file '${TS}' is missing")
endif()
file(READ "${TS}" _current)
if(_current MATCHES "type=\"(unfinished|vanished|obsolete)\"")
  message(FATAL_ERROR "${TS} has ${CMAKE_MATCH_1} entries")
endif()
# An empty translation shows the English text in the Spanish GUI without being marked unfinished (lupdate leaves it so after some edits).
if(_current MATCHES "<translation[^>]*>[ \t\r\n]*</translation>" OR _current MATCHES "<translation[^>]*/>"
   OR _current MATCHES "<numerusform[^>]*>[ \t\r\n]*</numerusform>" OR _current MATCHES "<numerusform[^>]*/>")
  message(FATAL_ERROR "${TS} has empty translations")
endif()
if(LUPDATE AND SRC AND WORK)
  file(MAKE_DIRECTORY "${WORK}")
  set(_copy "${WORK}/regenerated.ts")
  file(COPY_FILE "${TS}" "${_copy}")
  execute_process(COMMAND "${LUPDATE}" "${SRC}" -locations none -no-obsolete -source-language en -target-language es
                          -ts "${_copy}"
                  RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "lupdate failed: ${_err}")
  endif()
  file(READ "${_copy}" _regenerated)
  # Location lines carry line numbers that change with every edit; they are not part of the comparison.
  string(REGEX REPLACE "[ \t]*<location[^\n]*\n" "" _a "${_current}")
  string(REGEX REPLACE "[ \t]*<location[^\n]*\n" "" _b "${_regenerated}")
  if(NOT _a STREQUAL _b)
    message(FATAL_ERROR "${TS} is out of date: run the murasaki_lupdate target and translate the new strings "
                        "(compare with ${_copy})")
  endif()
endif()
