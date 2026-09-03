! pio_sst_reader_f.F90
!
! ADIOS2 SST streaming test — Fortran reader (data read and verification).
! Paired with pio_sst_writer_f; launched via separate mpirun (background writer).
!
! Opens the SST stream written by pio_sst_writer_f, reads NFRAMES timesteps of
! a 1-D integer variable, verifies the values match the expected pattern, then
! closes the stream.
!
! Validates: FR-002, FR-003, FR-007, FR-008, FR-009, SC-002, SC-005, SC-006.

program pio_sst_reader_f

  use pio
  use pio_kinds, only : PIO_OFFSET_KIND
#ifndef NO_MPIMOD
  use mpi
#endif
  implicit none
#ifdef NO_MPIMOD
  include 'mpif.h'
#endif

  integer, parameter :: NDIMS           = 1
  integer, parameter :: NFRAMES         = 3
  integer, parameter :: ELEMENTS_PER_PE = 4

  type(iosystem_desc_t) :: iosys
  type(file_desc_t)     :: file
  type(io_desc_t)       :: iodesc
  type(var_desc_t)      :: vdesc
  integer(PIO_OFFSET_KIND), allocatable :: compdof(:)
  integer :: buf(ELEMENTS_PER_PE)
  integer :: my_rank, nprocs, gdim
  integer :: iotype, ret, t, i, ierr, errors
  integer :: expected
  integer(PIO_OFFSET_KIND) :: frame_num

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, my_rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)

  call pio_init(my_rank, MPI_COMM_WORLD, nprocs, 0, 1, PIO_rearr_subset, iosys)

  ! 1-D decomposition matching the writer: each rank owns ELEMENTS_PER_PE
  ! consecutive elements with 1-based global indices
  gdim = nprocs * ELEMENTS_PER_PE
  allocate(compdof(ELEMENTS_PER_PE))
  do i = 1, ELEMENTS_PER_PE
    compdof(i) = int(my_rank * ELEMENTS_PER_PE + i, PIO_OFFSET_KIND)
  end do
  call pio_initdecomp(iosys, PIO_int, (/gdim/), compdof, iodesc)
  deallocate(compdof)

  ! Open SST stream (reader side) — blocks until writer is available
  iotype = PIO_iotype_adios_sst
  ret = pio_openfile(iosys, file, iotype, "scorpio_sst_test_stream_f", PIO_NOWRITE)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST reader: pio_openfile failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ! Locate the variable written by the writer
  ret = pio_inq_varid(file, "data", vdesc)
  if (ret /= PIO_noerr) then
    write(0,'(a,i0)') "SST reader: pio_inq_varid failed, ret=", ret
    call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
  end if

  ! Read and verify NFRAMES timesteps
  errors = 0
  do t = 0, NFRAMES - 1
    frame_num = int(t, PIO_OFFSET_KIND)
    call pio_setframe(file, vdesc, frame_num)

    call pio_read_darray(file, vdesc, iodesc, buf, ret)
    if (ret /= PIO_noerr) then
      write(0,'(a,i0,a,i0)') "SST reader: pio_read_darray failed at frame ", t, ", ret=", ret
      call MPI_Abort(MPI_COMM_WORLD, ret, ierr)
    end if

    do i = 1, ELEMENTS_PER_PE
      expected = t * 1000 + my_rank * ELEMENTS_PER_PE + (i - 1)
      if (buf(i) /= expected) then
        write(0,'(a,i0,a,i0,a,i0,a,i0,a,i0)') &
          "MISMATCH: frame=", t, " rank=", my_rank, " i=", i, &
          ": got ", buf(i), " expected ", expected
        errors = errors + 1
      end if
    end do
  end do

  if (errors > 0) then
    write(0,'(a,i0,a,i0)') "rank=", my_rank, ": verification errors=", errors
    call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  end if

  call pio_freedecomp(iosys, iodesc)
  call pio_closefile(file)
  call pio_finalize(iosys, ret)
  call MPI_Finalize(ierr)

  if (my_rank == 0) &
    write(*,'(a,i0,a)') "SST Fortran reader finished — all ", NFRAMES, " frames verified OK."

end program pio_sst_reader_f
