! pio_sst_reader_f.F90
!
! ADIOS2 SST streaming test — Fortran reader (connectivity smoke test).
! Paired with pio_sst_writer_f; launched together via MPMD mpirun.
!
! Opens the SST stream written by pio_sst_writer_f, verifies the connection
! succeeds, then closes.  Full per-frame data verification requires the
! SCORPIO SST read path (see convergence tasks T038/T039).
!
! Validates: FR-002, FR-007, SC-002 from the feature spec.

program pio_sst_reader_f

  use pio
#ifndef NO_MPIMOD
  use mpi
#endif
  implicit none
#ifdef NO_MPIMOD
  include 'mpif.h'
#endif

  type(iosystem_desc_t) :: iosys
  type(file_desc_t)     :: file
  integer :: my_rank, nprocs
  integer :: iotype, ret, ierr

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, my_rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)

  call pio_init(my_rank, MPI_COMM_WORLD, nprocs, 0, 1, PIO_rearr_subset, iosys)

  ! Open SST stream (reader side) — blocks until writer is available
  iotype = PIO_iotype_adios_sst
  ret = pio_openfile(iosys, file, iotype, "scorpio_sst_test_stream_f", PIO_NOWRITE)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST reader: pio_openfile failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ! SST stream connected successfully.
  ! Full per-frame read/verify is deferred to convergence tasks T038/T039
  ! (SCORPIO SST read path not yet implemented).
  ! Close immediately to release the writer.
  call pio_closefile(file)

  call pio_finalize(iosys, ret)
  call MPI_Finalize(ierr)

  if (my_rank == 0) &
    write(*,'(a)') "SST Fortran reader finished — stream connection verified."

end program pio_sst_reader_f
