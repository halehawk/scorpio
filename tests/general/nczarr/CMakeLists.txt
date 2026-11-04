# NCZarr (PIO_IOTYPE_NETCDF4P_NCZARR) tests
# Requires PIO_ENABLE_NCZARR=ON and a parallel NetCDF-C built with NCZarr.

message(STATUS "===== Configuring SCORPIO NCZarr tests... =====")

include(SPIOUtils)
include(LibMPI)

set(NCZARR_TEST_TIMEOUT 120)

#===== Fortran: write a Zarr store with the NCZarr iotype and read it back =====
add_spio_executable(pio_nczarr_f FALSE Fortran
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_nczarr_f.F90)
add_dependencies(tests pio_nczarr_f)

# zarr-python check of the same store, when a Python with zarr is available
find_package(Python3 COMPONENTS Interpreter)
set(nczarr_have_zarr FALSE)
if (Python3_Interpreter_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import zarr, numpy"
                  RESULT_VARIABLE nczarr_zarr_import OUTPUT_QUIET ERROR_QUIET)
  if (nczarr_zarr_import EQUAL 0)
    set(nczarr_have_zarr TRUE)
  endif ()
endif ()
if (NOT nczarr_have_zarr)
  message(STATUS "Python with zarr not found, skipping the zarr-python checks of NCZarr stores")
endif ()

foreach (np 1 4)
  add_mpi_test(nczarr_f_np${np}
    EXECUTABLE ${CMAKE_CURRENT_BINARY_DIR}/pio_nczarr_f
    ARGUMENTS pio_nczarr_f_np${np}.zarr
    NUMPROCS ${np}
    TIMEOUT ${NCZARR_TEST_TIMEOUT})
  set_tests_properties(nczarr_f_np${np} PROPERTIES FIXTURES_SETUP nczarr_f_np${np})

  if (nczarr_have_zarr)
    add_test(NAME nczarr_f_np${np}_zarr_check
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_nczarr_f.py
              pio_nczarr_f_np${np}.zarr ${np})
    set_tests_properties(nczarr_f_np${np}_zarr_check PROPERTIES
      FIXTURES_REQUIRED nczarr_f_np${np}
      TIMEOUT ${NCZARR_TEST_TIMEOUT})
  endif ()
endforeach ()
