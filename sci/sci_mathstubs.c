/* cc_mathstubs.c -- stubs for the libm functions Musashi's m68kfpu.c references
 * (FSIN/FCOS/etc.). A 68000 never executes FPU instructions, so these are never
 * called; defining them avoids pulling in mathieeedoubbas.library on Amiga. */
double sin(double x){ (void)x; return 0.0; }
double cos(double x){ (void)x; return 0.0; }
double cexp(double x){ (void)x; return 0.0; }
