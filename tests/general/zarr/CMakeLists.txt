# Native Zarr (PIO_IOTYPE_ZARR) tests
# Requires PIO_ENABLE_ZARR=ON.

message(STATUS "===== Configuring SCORPIO Zarr tests... =====")

include(SPIOUtils)
include(LibMPI)

set(ZARR_TEST_TIMEOUT 120)

#===== Fortran: write a Zarr store with the Zarr iotype on all tasks =====
add_spio_executable(pio_zarr_f FALSE Fortran
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_zarr_f.F90)
add_dependencies(tests pio_zarr_f)

# zarr-python check of the store (the Zarr iotype is write only)
find_package(Python3 COMPONENTS Interpreter)
set(zarr_have_zarr FALSE)
if (Python3_Interpreter_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import zarr, numpy"
                  RESULT_VARIABLE zarr_zarr_import OUTPUT_QUIET ERROR_QUIET)
  if (zarr_zarr_import EQUAL 0)
    set(zarr_have_zarr TRUE)
  endif ()
endif ()
if (NOT zarr_have_zarr)
  message(STATUS "Python with zarr not found, skipping the zarr-python checks of Zarr stores")
endif ()

foreach (np 1 3 4)
  add_mpi_test(zarr_f_np${np}
    EXECUTABLE ${CMAKE_CURRENT_BINARY_DIR}/pio_zarr_f
    ARGUMENTS pio_zarr_f_np${np}.zarr
    NUMPROCS ${np}
    TIMEOUT ${ZARR_TEST_TIMEOUT})
  set_tests_properties(zarr_f_np${np} PROPERTIES FIXTURES_SETUP zarr_f_np${np})

  if (zarr_have_zarr)
    add_test(NAME zarr_f_np${np}_zarr_check
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_zarr_f.py
              pio_zarr_f_np${np}.zarr ${np})
    set_tests_properties(zarr_f_np${np}_zarr_check PROPERTIES
      FIXTURES_REQUIRED zarr_f_np${np}
      TIMEOUT ${ZARR_TEST_TIMEOUT})
  endif ()
endforeach ()
