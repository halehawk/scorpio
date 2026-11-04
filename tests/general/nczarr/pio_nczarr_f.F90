! pio_nczarr_f.F90
!
! Writes a Zarr store with the NCZarr iotype (PIO_iotype_netcdf4p_nczarr)
! from Fortran, then reads it back through SCORPIO and checks everything.
! check_nczarr_f.py checks the same store with zarr-python.
!
!   mpiexec -n <nprocs> ./pio_nczarr_f [store name (default pio_nczarr_f.zarr)]
!
! Contents (NetCDF order; Fortran order is reversed):
!   dims   time (unlimited, NFRAMES records), lev = NLEV, lon = nprocs * LON_PER_PE
!   lon(lon)          double  coordinate, written with pio_put_var
!   T(time, lev, lon) double  2-D (lev, lon) decomposition, chunks (1, 2, LON_PER_PE)
!   ps(time, lon)     real    1-D decomposition
!   attributes: global "title", T "units"/"long_name", ps "units"
! Each rank owns LON_PER_PE consecutive lon columns (all levels).
!
! NetCDF-C's NCZarr does not coordinate writes from several processes
! (each one rewrites the store directory and whole chunks), so this test
! uses a single I/O task; the rearranger gathers the data of all compute
! tasks onto it.

program pio_nczarr_f

  use pio
  use pio_kinds, only : PIO_OFFSET_KIND
#ifndef NO_MPIMOD
  use mpi
#endif
  implicit none
#ifdef NO_MPIMOD
  include 'mpif.h'
#endif

  integer, parameter :: NLEV = 3, LON_PER_PE = 4, NFRAMES = 4
  character(len=*), parameter :: TITLE = "SCORPIO NCZarr Fortran test"

  type(iosystem_desc_t) :: iosys
  type(file_desc_t)     :: file
  type(io_desc_t)       :: iodesc_t, iodesc_ps
  type(var_desc_t)      :: vd_lon, vd_t, vd_ps
  integer(PIO_OFFSET_KIND), allocatable :: compdof(:)
  integer(PIO_OFFSET_KIND) :: dimlen
  double precision, allocatable :: lonvals(:), lonread(:)
  double precision :: t(LON_PER_PE * NLEV), tread(LON_PER_PE * NLEV)
  real :: ps(LON_PER_PE), psread(LON_PER_PE)
  character(len=PIO_MAX_NAME) :: fname, sval
  integer :: my_rank, nprocs, nlon, iotype, ret, ierr, nerr, nerr_all
  integer :: dim_lon, dim_lev, dim_time, f, k, i

  call MPI_Init(ierr)
  call MPI_Comm_rank(MPI_COMM_WORLD, my_rank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)
  nlon = nprocs * LON_PER_PE
  nerr = 0

  fname = "pio_nczarr_f.zarr"
  if (command_argument_count() >= 1) call get_command_argument(1, fname)

  iotype = PIO_iotype_netcdf4p_nczarr
  if (.not. pio_iotype_available(iotype)) then
    if (my_rank == 0) write(0,'(a)') "NCZarr iotype not available in this SCORPIO build"
    call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  end if

  ! One I/O task (see the note above), box rearranger
  call pio_init(my_rank, MPI_COMM_WORLD, 1, 0, 1, PIO_rearr_box, iosys)

  ! T: (lon, lev) decomposition, 1-based offsets in Fortran order
  allocate(compdof(LON_PER_PE * NLEV))
  do k = 1, NLEV
    do i = 1, LON_PER_PE
      compdof((k - 1) * LON_PER_PE + i) = &
        int((k - 1) * nlon + my_rank * LON_PER_PE + i, PIO_OFFSET_KIND)
    end do
  end do
  call pio_initdecomp(iosys, PIO_double, (/nlon, NLEV/), compdof, iodesc_t)
  deallocate(compdof)

  ! ps: (lon) decomposition
  allocate(compdof(LON_PER_PE))
  do i = 1, LON_PER_PE
    compdof(i) = int(my_rank * LON_PER_PE + i, PIO_OFFSET_KIND)
  end do
  call pio_initdecomp(iosys, PIO_real, (/nlon/), compdof, iodesc_ps)
  deallocate(compdof)

  allocate(lonvals(nlon), lonread(nlon))
  do i = 1, nlon
    lonvals(i) = 0.5d0 * (i - 1)
  end do

  !---------------------------------------------------------------- write
  call check(pio_createfile(iosys, file, iotype, trim(fname), PIO_CLOBBER), "pio_createfile")
  call check(pio_def_dim(file, "lon", nlon, dim_lon), "pio_def_dim lon")
  call check(pio_def_dim(file, "lev", NLEV, dim_lev), "pio_def_dim lev")
  call check(pio_def_dim(file, "time", PIO_unlimited, dim_time), "pio_def_dim time")

  call check(pio_def_var(file, "lon", PIO_double, (/dim_lon/), vd_lon), "pio_def_var lon")
  call check(pio_def_var(file, "T", PIO_double, (/dim_lon, dim_lev, dim_time/), vd_t), &
             "pio_def_var T")
  call check(pio_def_var(file, "ps", PIO_real, (/dim_lon, dim_time/), vd_ps), "pio_def_var ps")
  ! Chunk sizes in Fortran order (lon, lev, time) -> Zarr chunks (1, 2, LON_PER_PE)
  call check(pio_def_var_chunking(file, vd_t, PIO_chunked, (/LON_PER_PE, 2, 1/)), &
             "pio_def_var_chunking T")

  call check(pio_put_att(file, PIO_global, "title", TITLE), "pio_put_att title")
  call check(pio_put_att(file, vd_t, "units", "K"), "pio_put_att T units")
  call check(pio_put_att(file, vd_t, "long_name", "air temperature"), "pio_put_att T long_name")
  call check(pio_put_att(file, vd_ps, "units", "hPa"), "pio_put_att ps units")
  call check(pio_enddef(file), "pio_enddef")

  call check(pio_put_var(file, vd_lon, lonvals), "pio_put_var lon")

  do f = 1, NFRAMES
    call fill_frame(f, t, ps)
    ! pio_setframe takes 1-based frame numbers
    call pio_setframe(file, vd_t, int(f, PIO_OFFSET_KIND))
    call pio_write_darray(file, vd_t, iodesc_t, t, ret)
    call check(ret, "pio_write_darray T")
    call pio_setframe(file, vd_ps, int(f, PIO_OFFSET_KIND))
    call pio_write_darray(file, vd_ps, iodesc_ps, ps, ret)
    call check(ret, "pio_write_darray ps")
  end do
  call pio_closefile(file)

  !------------------------------------------------- read back via SCORPIO
  call check(pio_openfile(iosys, file, iotype, trim(fname), PIO_nowrite), "pio_openfile")

  call check(pio_inq_dimid(file, "lon", dim_lon), "pio_inq_dimid lon")
  call check(pio_inq_dimlen(file, dim_lon, dimlen), "pio_inq_dimlen lon")
  call expect(dimlen == nlon, "lon length")
  call check(pio_inq_dimid(file, "lev", dim_lev), "pio_inq_dimid lev")
  call check(pio_inq_dimlen(file, dim_lev, dimlen), "pio_inq_dimlen lev")
  call expect(dimlen == NLEV, "lev length")
  call check(pio_inq_dimid(file, "time", dim_time), "pio_inq_dimid time")
  call check(pio_inq_dimlen(file, dim_time, dimlen), "pio_inq_dimlen time")
  call expect(dimlen == NFRAMES, "time length (records written)")

  call check(pio_inq_varid(file, "lon", vd_lon), "pio_inq_varid lon")
  call check(pio_inq_varid(file, "T", vd_t), "pio_inq_varid T")
  call check(pio_inq_varid(file, "ps", vd_ps), "pio_inq_varid ps")

  sval = ""
  call check(pio_get_att(file, PIO_global, "title", sval), "pio_get_att title")
  call expect(trim(sval) == TITLE, "global attribute title")
  sval = ""
  call check(pio_get_att(file, vd_t, "units", sval), "pio_get_att T units")
  call expect(trim(sval) == "K", "T units")
  sval = ""
  call check(pio_get_att(file, vd_ps, "units", sval), "pio_get_att ps units")
  call expect(trim(sval) == "hPa", "ps units")

  lonread = -1.0d0
  call check(pio_get_var(file, vd_lon, lonread), "pio_get_var lon")
  call expect(all(lonread == lonvals), "lon values")

  do f = 1, NFRAMES
    call fill_frame(f, t, ps)
    tread = -1.0d0
    psread = -1.0
    call pio_setframe(file, vd_t, int(f, PIO_OFFSET_KIND))
    call pio_read_darray(file, vd_t, iodesc_t, tread, ret)
    call check(ret, "pio_read_darray T")
    call expect(all(tread == t), "T values")
    call pio_setframe(file, vd_ps, int(f, PIO_OFFSET_KIND))
    call pio_read_darray(file, vd_ps, iodesc_ps, psread, ret)
    call check(ret, "pio_read_darray ps")
    call expect(all(psread == ps), "ps values")
  end do
  call pio_closefile(file)

  call pio_freedecomp(iosys, iodesc_t)
  call pio_freedecomp(iosys, iodesc_ps)
  call pio_finalize(iosys, ret)

  call MPI_Allreduce(nerr, nerr_all, 1, MPI_INTEGER, MPI_SUM, MPI_COMM_WORLD, ierr)
  if (my_rank == 0) then
    if (nerr_all == 0) then
      write(*,'(a,a,a,i0,a)') "PASS: wrote and read back ", trim(fname), " (", nprocs, " ranks)"
    else
      write(*,'(a,i0,a)') "FAIL: ", nerr_all, " check(s) failed"
    end if
  end if
  if (nerr_all /= 0) call MPI_Abort(MPI_COMM_WORLD, 1, ierr)
  call MPI_Finalize(ierr)

contains

  ! Values of frame f for this rank's lon columns:
  !   T(lon, lev)  = 250 + (f-1) + 10*(lev-1) + 0.01*(lon-1)
  !   ps(lon)      = 1000 + (f-1) + 0.5*(lon-1)
  subroutine fill_frame(f, t, ps)
    integer, intent(in) :: f
    double precision, intent(out) :: t(:)
    real, intent(out) :: ps(:)
    integer :: k, i, glon
    do k = 1, NLEV
      do i = 1, LON_PER_PE
        glon = my_rank * LON_PER_PE + i
        t((k - 1) * LON_PER_PE + i) = 250.0d0 + (f - 1) + 10.0d0 * (k - 1) + 0.01d0 * (glon - 1)
      end do
    end do
    do i = 1, LON_PER_PE
      glon = my_rank * LON_PER_PE + i
      ps(i) = 1000.0 + (f - 1) + 0.5 * (glon - 1)
    end do
  end subroutine fill_frame

  subroutine check(code, what)
    integer, intent(in) :: code
    character(len=*), intent(in) :: what
    integer :: ierr2
    if (code /= PIO_noerr) then
      write(0,'(a,i0,a,a,a,i0)') "rank ", my_rank, ": ", what, " failed, ret=", code
      call MPI_Abort(MPI_COMM_WORLD, 1, ierr2)
    end if
  end subroutine check

  subroutine expect(ok, what)
    logical, intent(in) :: ok
    character(len=*), intent(in) :: what
    if (.not. ok) then
      write(0,'(a,i0,a,a)') "rank ", my_rank, ": wrong ", what
      nerr = nerr + 1
    end if
  end subroutine expect

end program pio_nczarr_f
