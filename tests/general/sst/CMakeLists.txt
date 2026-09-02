# ADIOS2 SST streaming engine tests
# Requires WITH_ADIOS2=ON and an ADIOS2 build with SST transport support.

if (WITH_ADIOS2)

message(STATUS "===== Configuring SCORPIO ADIOS2 SST streaming tests... =====")

include(SPIOUtils)
include(PIOTest)
include(LibMPI)

set(DEFAULT_SST_TEST_TIMEOUT 300)

#===== pio_sst_writer / pio_sst_reader (C, separate writer+reader executables) =====
# Launched together via MPMD mpirun through a configured shell script so the SST
# rendezvous happens within a single mpirun invocation.
# 2 ranks run the writer, 2 ranks run the reader.
add_spio_executable(pio_sst_writer TRUE C
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_sst_writer.c)
target_include_directories(pio_sst_writer PRIVATE
    ${PROJECT_SOURCE_DIR}/src/clib
    ${PROJECT_BINARY_DIR}/src/clib)
add_dependencies(tests pio_sst_writer)

add_spio_executable(pio_sst_reader TRUE C
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_sst_reader.c)
target_include_directories(pio_sst_reader PRIVATE
    ${PROJECT_SOURCE_DIR}/src/clib
    ${PROJECT_BINARY_DIR}/src/clib)
add_dependencies(tests pio_sst_reader)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/run_sst_c_paired.sh.in
    ${CMAKE_CURRENT_BINARY_DIR}/run_sst_c_paired.sh
    @ONLY)
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/run_sst_c_paired.sh
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)

add_test(NAME sst_endtoend
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/run_sst_c_paired.sh)
set_tests_properties(sst_endtoend PROPERTIES
    TIMEOUT ${DEFAULT_SST_TEST_TIMEOUT})

#===== pio_sst_writer_f / pio_sst_reader_f (Fortran, separate writer+reader executables) =====
# Launched together via MPMD mpirun through a configured shell script.
if (CMAKE_Fortran_COMPILER_WORKS OR CMAKE_Fortran_COMPILER)

  add_spio_executable(pio_sst_writer_f FALSE Fortran
      ${CMAKE_CURRENT_SOURCE_DIR}/pio_sst_writer_f.F90)
  add_dependencies(tests pio_sst_writer_f)

  add_spio_executable(pio_sst_reader_f FALSE Fortran
      ${CMAKE_CURRENT_SOURCE_DIR}/pio_sst_reader_f.F90)
  add_dependencies(tests pio_sst_reader_f)

  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/run_sst_f_paired.sh.in
      ${CMAKE_CURRENT_BINARY_DIR}/run_sst_f_paired.sh
      @ONLY)
  file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/run_sst_f_paired.sh
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE)

  add_test(NAME sst_endtoend_f
      COMMAND ${CMAKE_CURRENT_BINARY_DIR}/run_sst_f_paired.sh)
  set_tests_properties(sst_endtoend_f PROPERTIES
      TIMEOUT ${DEFAULT_SST_TEST_TIMEOUT})

endif () # Fortran compiler available

endif (WITH_ADIOS2)
