/* ymdeltat.c -- YM Delta-T / YM2610 ADPCM-B external-ROM playback. */
#include "driver.h"      /* UINT8 / INT32 / ... typedefs */
#include "ymdeltat.h"

#define YM_DELTAT_SHIFT        16
#define YM_DELTAT_DELTA_MAX    24576
#define YM_DELTAT_DELTA_MIN    127
#define YM_DELTAT_DELTA_DEF    127
#define YM_DELTAT_DECODE_RANGE 32768
#define YM_DELTAT_DECODE_MIN   (-(YM_DELTAT_DECODE_RANGE))
#define YM_DELTAT_DECODE_MAX   ((YM_DELTAT_DECODE_RANGE)-1)

static const INT32 tableB1[16] = { 1,3,5,7,9,11,13,15, -1,-3,-5,-7,-9,-11,-13,-15 };
static const INT32 tableB2[16] = { 57,57,57,57,77,102,128,153, 57,57,57,57,77,102,128,153 };
static const UINT8 dram_rightshift[4] = { 3,0,0,0 };

static void clamp_i(INT32 *v, INT32 max, INT32 min){ if(*v>max)*v=max; else if(*v<min)*v=min; }
static void status_set(YM_DELTAT *D, UINT8 bit){ if(D->status_set_handler && bit) D->status_set_handler((UINT8)D->status_change_which_chip, bit); }
static void status_reset(YM_DELTAT *D, UINT8 bit){ if(D->status_reset_handler && bit) D->status_reset_handler((UINT8)D->status_change_which_chip, bit); }

UINT8 YM_DELTAT_ADPCM_Read(YM_DELTAT *D){
    UINT8 v = 0;
    if((D->portstate & 0xe0) == 0x20){
        if(D->memread){ D->now_addr = D->start << 1; D->memread--; return 0; }
        if(D->now_addr != (D->end << 1) && D->memory){
            v = D->memory[D->now_addr >> 1];
            D->now_addr += 2;
            status_reset(D, D->status_change_BRDY_bit);
            status_set(D, D->status_change_BRDY_bit);
        } else {
            status_set(D, D->status_change_EOS_bit);
        }
    }
    return v;
}

void YM_DELTAT_ADPCM_Write(YM_DELTAT *D, int r, int v){
    if(r < 0 || r >= 0x10) return;
    D->reg[r] = (UINT8)v;
    switch(r){
    case 0x00:
        if(D->emulation_mode == YM_DELTAT_EMULATION_MODE_YM2610) v |= 0x20;
        D->portstate = (UINT8)(v & (0x80|0x40|0x20|0x10|0x01));
        if(D->portstate & 0x80){
            D->PCM_BSY = 1;
            D->now_step = 0;
            D->acc = 0;
            D->prev_acc = 0;
            D->adpcml = 0;
            D->adpcmd = YM_DELTAT_DELTA_DEF;
            D->now_data = 0;
        }
        if(D->portstate & 0x20){
            D->now_addr = D->start << 1;
            D->memread = 2;
            if(!D->memory){
                D->portstate = 0;
                D->PCM_BSY = 0;
            } else {
                if(D->end >= D->memory_size) D->end = D->memory_size ? D->memory_size - 1 : 0;
                if(D->start >= D->memory_size){ D->portstate = 0; D->PCM_BSY = 0; }
            }
        } else {
            D->now_addr = 0;
        }
        if(D->portstate & 0x01){
            D->portstate = 0;
            D->PCM_BSY = 0;
            status_set(D, D->status_change_BRDY_bit);
        }
        break;
    case 0x01:
        if(D->emulation_mode == YM_DELTAT_EMULATION_MODE_YM2610) v |= 0x01;
        if(D->output_pointer) D->pan = &D->output_pointer[(v >> 6) & 3];
        if((D->control2 & 3) != (v & 3)){
            if(D->DRAMportshift != dram_rightshift[v & 3]){
                D->DRAMportshift = dram_rightshift[v & 3];
                D->start = (UINT32)((D->reg[3] * 0x0100u | D->reg[2]) << (D->portshift - D->DRAMportshift));
                D->end   = (UINT32)((D->reg[5] * 0x0100u | D->reg[4]) << (D->portshift - D->DRAMportshift));
                D->end  += (UINT32)((1u << (D->portshift - D->DRAMportshift)) - 1u);
                D->limit = (UINT32)((D->reg[0xd] * 0x0100u | D->reg[0xc]) << (D->portshift - D->DRAMportshift));
            }
        }
        D->control2 = (UINT8)v;
        break;
    case 0x02: case 0x03:
        D->start = (UINT32)((D->reg[3] * 0x0100u | D->reg[2]) << (D->portshift - D->DRAMportshift));
        break;
    case 0x04: case 0x05:
        D->end = (UINT32)((D->reg[5] * 0x0100u | D->reg[4]) << (D->portshift - D->DRAMportshift));
        D->end += (UINT32)((1u << (D->portshift - D->DRAMportshift)) - 1u);
        break;
    case 0x08:
        if((D->portstate & 0xe0) == 0x80){
            D->CPU_data = (UINT8)v;
            status_reset(D, D->status_change_BRDY_bit);
        }
        break;
    case 0x09: case 0x0a:
        D->delta = (UINT32)(D->reg[0x0a] * 0x0100u | D->reg[0x09]);
        D->step = D->delta; /* this fm.c is FP-free and fixes OPN_ST.freqbase at 1.0 */
        break;
    case 0x0b:
        D->volume = (INT32)((v & 0xff) * (D->output_range / 256) / YM_DELTAT_DECODE_RANGE);
        break;
    case 0x0c: case 0x0d:
        D->limit = (UINT32)((D->reg[0x0d] * 0x0100u | D->reg[0x0c]) << (D->portshift - D->DRAMportshift));
        break;
    default:
        break;
    }
}

void YM_DELTAT_ADPCM_Reset(YM_DELTAT *D, int pan, int mode){
    D->now_addr = 0;
    D->now_step = 0;
    D->step = 0;
    D->start = 0;
    D->end = 0;
    D->limit = ~0u;
    D->volume = 0;
    D->pan = D->output_pointer ? &D->output_pointer[pan] : 0;
    D->acc = 0;
    D->prev_acc = 0;
    D->adpcmd = YM_DELTAT_DELTA_DEF;
    D->adpcml = 0;
    D->emulation_mode = (UINT8)mode;
    D->portstate = (mode == YM_DELTAT_EMULATION_MODE_YM2610) ? 0x20 : 0;
    D->control2 = (mode == YM_DELTAT_EMULATION_MODE_YM2610) ? 0x01 : 0;
    D->DRAMportshift = dram_rightshift[D->control2 & 3];
    status_set(D, D->status_change_BRDY_bit);
}

static void synth_external(YM_DELTAT *D){
    UINT32 step;
    int data;
    if(!D->memory || !D->pan) return;
    D->now_step += D->step;
    if(D->now_step >= (1u << YM_DELTAT_SHIFT)){
        step = D->now_step >> YM_DELTAT_SHIFT;
        D->now_step &= (1u << YM_DELTAT_SHIFT) - 1u;
        do {
            if(D->now_addr == (D->limit << 1)) D->now_addr = 0;
            if(D->now_addr == (D->end << 1)){
                if(D->portstate & 0x10){
                    D->now_addr = D->start << 1;
                    D->acc = 0;
                    D->adpcmd = YM_DELTAT_DELTA_DEF;
                    D->prev_acc = 0;
                } else {
                    status_set(D, D->status_change_EOS_bit);
                    D->PCM_BSY = 0;
                    D->portstate = 0;
                    D->adpcml = 0;
                    D->prev_acc = 0;
                    return;
                }
            }
            if(D->now_addr & 1) data = D->now_data & 0x0f;
            else { D->now_data = D->memory[D->now_addr >> 1]; data = D->now_data >> 4; }
            D->now_addr++;
            D->now_addr &= ((1u << 25) - 1u);
            D->prev_acc = D->acc;
            D->acc += (tableB1[data] * D->adpcmd / 8);
            clamp_i(&D->acc, YM_DELTAT_DECODE_MAX, YM_DELTAT_DECODE_MIN);
            D->adpcmd = (D->adpcmd * tableB2[data]) / 64;
            clamp_i(&D->adpcmd, YM_DELTAT_DELTA_MAX, YM_DELTAT_DELTA_MIN);
        } while(--step);
    }
    D->adpcml = D->prev_acc * (INT32)((1u << YM_DELTAT_SHIFT) - D->now_step);
    D->adpcml += D->acc * (INT32)D->now_step;
    D->adpcml = (D->adpcml >> YM_DELTAT_SHIFT) * D->volume;
    *D->pan += D->adpcml;
}

void YM_DELTAT_ADPCM_CALC(YM_DELTAT *D){
    if((D->portstate & 0xe0) == 0xa0) synth_external(D);
}
void  YM_DELTAT_BRDY_callback(YM_DELTAT *D){ (void)D; }
void  YM_DELTAT_postload(YM_DELTAT *D, UINT8 *regs){ D->volume=0; for(int r=1;r<16;r++) YM_DELTAT_ADPCM_Write(D,r,regs[r]); D->reg[0]=regs[0]; if(D->memory) D->now_data=D->memory[D->now_addr>>1]; }
void  YM_DELTAT_savestate(const char *n, int num, YM_DELTAT *D){ (void)n;(void)num;(void)D; }
