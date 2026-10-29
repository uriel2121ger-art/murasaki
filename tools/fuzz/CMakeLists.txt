# Fuzz targets of the import parsers (tools/fuzz/fuzz_import.cpp). Built only with -DMURASAKI_BUILD_FUZZ=ON, in a build directory of its own with sanitizers.
add_executable(fuzz_import fuzz_import.cpp)
target_link_libraries(fuzz_import PRIVATE murasaki_import_logic Qt6::Core)
if(MURASAKI_LIBFUZZER)
  # clang: one libFuzzer binary per target, e.g. -DMURASAKI_LIBFUZZER=ON with CMAKE_CXX_COMPILER=clang++
  foreach(_t zip mddo)
    add_executable(fuzz_${_t} fuzz_import.cpp)
    target_compile_definitions(fuzz_${_t} PRIVATE MURASAKI_LIBFUZZER $<$<STREQUAL:${_t},mddo>:MURASAKI_FUZZ_MDDO>)
    target_compile_options(fuzz_${_t} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${_t} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_${_t} PRIVATE murasaki_import_logic Qt6::Core)
  endforeach()
endif()
