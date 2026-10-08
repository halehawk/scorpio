/* Shared sizes and expected values for pio_sst_multivar_writer/reader. */
#ifndef PIO_SST_MULTIVAR_H
#define PIO_SST_MULTIVAR_H

#define NFRAMES         10
#define NY              3
#define ELEMENTS_PER_PE 4
#define MV_STREAM_NAME  "scorpio_sst_multivar_stream"

/* x is the global (0-based) x index; all values are exact in double/int */
static inline double mv_temp(int f, int x)        { return f * 1000.0 + x + 0.5; }
static inline double mv_salt(int f, int x)        { return -(f * 1000.0 + x) - 0.25; }
static inline int    mv_pres(int f, int y, int x) { return f * 10000 + y * 100 + x; }

#endif
