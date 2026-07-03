#ifndef YM_DELTAT_H
#define YM_DELTAT_H
/* YM Delta-T / YM2610 ADPCM-B state. This is the C shim used by the old fm.c
 * core in this tree; Continental Circus uses the external ROM playback path. */

#define YM_DELTAT_EMULATION_MODE_NORMAL 0
#define YM_DELTAT_EMULATION_MODE_YM2610 1

typedef void (*STATUS_CHANGE_HANDLER)(UINT8 which, UINT8 changebits);

typedef struct deltat_adpcm_state {
    UINT8  *memory;
    INT32  *output_pointer;     /* pointer to the output stream accumulator */
    INT32  *pan;                /* one of output_pointer[OUTD_*] */
    int     portshift;
    int     DRAMportshift;
    int     output_range;
    UINT32  memory_size;
    UINT32  now_addr;
    UINT32  now_step;
    UINT32  step;
    UINT32  start;
    UINT32  limit;
    UINT32  end;
    UINT32  delta;
    INT32   volume;
    INT32   acc;
    INT32   adpcmd;
    INT32   adpcml;
    INT32   prev_acc;
    UINT8   now_data;
    UINT8   CPU_data;
    UINT8   portstate;
    UINT8   control2;
    UINT8   memread;
    UINT8   PCM_BSY;            /* 1 if ADPCM is playing */
    UINT8   emulation_mode;
    double  freqbase;
    double  write_time, read_time;
    int     status_change_which_chip;
    UINT8   status_change_EOS_bit, status_change_BRDY_bit, status_change_ZERO_bit;
    STATUS_CHANGE_HANDLER status_set_handler;
    STATUS_CHANGE_HANDLER status_reset_handler;
    UINT8   reg[16];
} YM_DELTAT;

void  YM_DELTAT_ADPCM_Write(YM_DELTAT *DELTAT, int r, int v);
UINT8 YM_DELTAT_ADPCM_Read(YM_DELTAT *DELTAT);
void  YM_DELTAT_ADPCM_Reset(YM_DELTAT *DELTAT, int pan, int emulation_mode);
void  YM_DELTAT_ADPCM_CALC(YM_DELTAT *DELTAT);
void  YM_DELTAT_BRDY_callback(YM_DELTAT *DELTAT);
void  YM_DELTAT_postload(YM_DELTAT *DELTAT, UINT8 *regs);
void  YM_DELTAT_savestate(const char *statename, int num, YM_DELTAT *DELTAT);

#endif
