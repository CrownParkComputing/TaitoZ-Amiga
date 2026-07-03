/* chq_m68k_gate.c -- HOST-ONLY validation gate for the NATIVE (Rust->m68k) Chase HQ
 * sound-Z80 transcode: instead of linking the host-compiled crate, this shim LOADS the
 * actual m68k ELF object (the exact bytes the Amiga runs) and EXECUTES it under a
 * second, symbol-renamed Musashi 68000 instance, wired into the same cc_hosttest gate.
 *
 * Purpose: the normal host gate (build_host_native.sh) proves the RUST SOURCE is
 * bit-exact vs the z80emu interpreter, but it is compiled by host x86 codegen -- it
 * can never catch an LLVM m68k BACKEND miscompile.  This gate runs the true m68k
 * machine code, so
 *     interpreter RAW == host-native RAW == m68k-gate RAW
 * proves generator + m68k codegen + (independently verified) link path end to end.
 *
 * Build: build_host_m68kgate.sh.  The second Musashi copy is objcopy
 * --redefine-syms'd to a g68k_ prefix so it coexists with the game's dual-68000
 * Musashi (cc_machine.c) in one process.
 *
 * Env: CHQ_M68KOBJ=<path to the (already _-prefixed) m68k cgu ELF object>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m68k.h"   /* m68k_register_t / M68K_REG_* / M68K_CPU_TYPE_68000 enums only */

/* ---- renamed Musashi API (see build_host_m68kgate.sh redefine map) ---- */
extern void g68k_m68k_init(void);
extern void g68k_m68k_set_cpu_type(unsigned int cpu_type);
extern void g68k_m68k_pulse_reset(void);
extern int  g68k_m68k_execute(int num_cycles);
extern void g68k_m68k_end_timeslice(void);
extern unsigned int g68k_m68k_get_reg(void *context, m68k_register_t reg);
extern void g68k_m68k_set_reg(m68k_register_t reg, unsigned int value);
extern void g68k_m68k_set_instr_hook_callback(void (*callback)(unsigned int pc));

/* ---- C hooks the transcode imports (defined in cc_audio.c, CHQ_AUDIO_NATIVE) ---- */
extern uint8_t chq_aud_ym_read(uint32_t a);
extern void    chq_aud_ym_write(uint32_t a, uint8_t v);
extern uint8_t chq_aud_syt_read(void);
extern void    chq_aud_syt_port_w(uint8_t v);
extern void    chq_aud_syt_comm_w(uint8_t v);
extern void    chq_aud_bank_w(uint8_t v);
extern void    chq_aud_trace(unsigned short pc);

/* mirror of cc_audio.c's ChqZ80Native (host layout: mem at offset 48 on 64-bit) */
typedef struct {
    uint8_t  b, c, d, e, h, l, a, f;
    uint8_t  ixh, ixl, iyh, iyl;
    uint16_t sp, pc;
    uint16_t bc_alt, de_alt, hl_alt, af_alt;
    uint8_t  i, r, iff1, iff2;
    uint8_t  im, halted, stop, pad0;
    uint32_t cycles, budget;
    uint16_t badpc, pad1;
    uint8_t *mem;
} ChqZ80Native;

/* ---- guest 68000 memory map (24-bit space, one flat array) ---- */
#define G_SIZE     0x1000000u
#define CODE_BASE  0x00100000u
#define STATE_ADDR 0x00E00000u
#define MEM_ADDR   0x00E10000u   /* 64KB Z80 image copy */
#define STACK_TOP  0x00EF0000u
#define HOOK_BASE  0x00F00000u   /* hook i lives at HOOK_BASE + i*16 (an RTS) */
#define RET_MAGIC  0x00F01000u   /* chq_aud_run's fake return address (bra .self) */

static uint8_t G[G_SIZE];
static uint32_t gate_entry;      /* guest address of _chq_aud_run */
static int gate_ready, gate_done;
static uint8_t *host_mem;        /* live s->mem for the current call (bank refresh) */
static unsigned long long gate_calls, gate_cycles;

static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static void wbe32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void wbe16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }

/* ---- renamed Musashi memory callbacks: flat big-endian guest RAM ---- */
unsigned int g68k_m68k_read_memory_8 (unsigned int a){ return G[a & 0xffffff]; }
unsigned int g68k_m68k_read_memory_16(unsigned int a){ return be16(G + (a & 0xffffff)); }
unsigned int g68k_m68k_read_memory_32(unsigned int a){ return be32(G + (a & 0xffffff)); }
unsigned int g68k_m68k_read_immediate_16(unsigned int a){ return be16(G + (a & 0xffffff)); }
unsigned int g68k_m68k_read_immediate_32(unsigned int a){ return be32(G + (a & 0xffffff)); }
unsigned int g68k_m68k_read_pcrelative_8 (unsigned int a){ return G[a & 0xffffff]; }
unsigned int g68k_m68k_read_pcrelative_16(unsigned int a){ return be16(G + (a & 0xffffff)); }
unsigned int g68k_m68k_read_pcrelative_32(unsigned int a){ return be32(G + (a & 0xffffff)); }
void g68k_m68k_write_memory_8 (unsigned int a, unsigned int v){ G[a & 0xffffff] = (uint8_t)v; }
void g68k_m68k_write_memory_16(unsigned int a, unsigned int v){ wbe16(G + (a & 0xffffff), (uint16_t)v); }
void g68k_m68k_write_memory_32(unsigned int a, unsigned int v){ wbe32(G + (a & 0xffffff), v); }

/* hook indices == order in hook_names[] */
static const char *hook_names[] = {
    "_chq_aud_ym_read", "_chq_aud_ym_write", "_chq_aud_syt_read",
    "_chq_aud_syt_port_w", "_chq_aud_syt_comm_w", "_chq_aud_bank_w",
    "_abort", "_chq_aud_trace",
};
#define N_HOOKS (sizeof hook_names / sizeof hook_names[0])

/* CHQ_GATEPROF=<file>: per-16-byte-bucket guest PC histogram (one count per executed
 * instruction), dumped at exit; merge with the CHQ_GATESYMS symbol dump to see where
 * the emulated 68k time goes. */
static unsigned *prof_hist;   /* [G_SIZE>>4] */
static void prof_dump(void){
    const char *fn = getenv("CHQ_GATEPROF");
    if(!fn || !prof_hist) return;
    FILE *f = fopen(fn, "w");
    if(!f) return;
    for(uint32_t i = 0; i < (G_SIZE >> 4); i++)
        if(prof_hist[i]) fprintf(f, "%08x %u\n", i << 4, prof_hist[i]);
    fclose(f);
}
static void gate_hook(unsigned int pc){
    if(prof_hist) prof_hist[(pc & 0xffffff) >> 4]++;
    if(pc < HOOK_BASE || pc > RET_MAGIC) return;
    if(pc == RET_MAGIC){ gate_done = 1; g68k_m68k_end_timeslice(); return; }
    unsigned idx = (pc - HOOK_BASE) >> 4;
    uint32_t sp = g68k_m68k_get_reg(NULL, M68K_REG_SP);
    uint32_t a1 = be32(G + ((sp + 4) & 0xffffff));
    uint32_t a2 = be32(G + ((sp + 8) & 0xffffff));
    switch(idx){
        case 0: g68k_m68k_set_reg(M68K_REG_D0, chq_aud_ym_read(a1)); break;
        case 1: chq_aud_ym_write(a1, (uint8_t)a2); break;
        case 2: g68k_m68k_set_reg(M68K_REG_D0, chq_aud_syt_read()); break;
        case 3: chq_aud_syt_port_w((uint8_t)a1); break;
        case 4: chq_aud_syt_comm_w((uint8_t)a1); break;
        case 5: chq_aud_bank_w((uint8_t)a1);
                /* keep the guest's banked-window copy coherent mid-slice */
                if(host_mem) memcpy(G + MEM_ADDR + 0x4000, host_mem + 0x4000, 0x4000);
                break;
        case 6: fprintf(stderr, "m68k-gate: guest called abort()\n"); exit(99);
        case 7: chq_aud_trace((unsigned short)a1); break;
        default: fprintf(stderr, "m68k-gate: unknown hook pc=%08x\n", pc); exit(98);
    }
    /* the RTS placed at the hook address now executes and returns to the caller */
}

/* ---- minimal big-endian ELF32 ET_REL loader (R_68K_32 only) ---- */
static void gate_load_elf(const char *path){
    FILE *f = fopen(path, "rb");
    if(!f){ fprintf(stderr, "m68k-gate: cannot open %s\n", path); exit(97); }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *e = malloc((size_t)fsz);
    if(fread(e, 1, (size_t)fsz, f) != (size_t)fsz){ fprintf(stderr, "m68k-gate: short read\n"); exit(97); }
    fclose(f);
    if(memcmp(e, "\x7f""ELF\x01\x02", 6) != 0){ fprintf(stderr, "m68k-gate: not a BE ELF32: %s\n", path); exit(97); }
    uint32_t shoff = be32(e + 0x20);
    unsigned shentsize = be16(e + 0x2e), shnum = be16(e + 0x30);
    const uint8_t *sh = e + shoff;
    #define SH(i, off) (sh + (size_t)(i) * shentsize + (off))
    uint32_t *secaddr = calloc(shnum, sizeof(uint32_t));

    /* pass 1: place SHF_ALLOC sections at CODE_BASE.. and copy PROGBITS */
    uint32_t cur = CODE_BASE;
    for(unsigned i = 0; i < shnum; i++){
        uint32_t type = be32(SH(i,4)), flags = be32(SH(i,8));
        uint32_t off = be32(SH(i,0x10)), size = be32(SH(i,0x14)), align = be32(SH(i,0x20));
        if(!(flags & 2)) continue;               /* !SHF_ALLOC */
        if(align < 4) align = 4;
        cur = (cur + align - 1) & ~(align - 1);
        secaddr[i] = cur;
        if(cur + size >= STATE_ADDR){ fprintf(stderr, "m68k-gate: image too big\n"); exit(97); }
        if(type == 1 /*PROGBITS*/) memcpy(G + cur, e + off, size);
        else memset(G + cur, 0, size);           /* NOBITS */
        cur += size;
    }

    /* find symtab + strtab */
    uint32_t symoff = 0, symnum = 0, stroff = 0;
    unsigned symsec = 0;
    for(unsigned i = 0; i < shnum; i++){
        if(be32(SH(i,4)) == 2 /*SYMTAB*/){
            symsec = i;
            symoff = be32(SH(i,0x10));
            symnum = be32(SH(i,0x14)) / 16;
            unsigned link = be32(SH(i,0x18));
            stroff = be32(SH(link,0x10));
        }
    }
    if(!symoff){ fprintf(stderr, "m68k-gate: no symtab\n"); exit(97); }
    (void)symsec;
    #define SYM(i, off) (e + symoff + (size_t)(i) * 16 + (off))

    /* symbol -> guest address (UND symbols map to hook slots by name) */
    uint32_t *symaddr = calloc(symnum, sizeof(uint32_t));
    for(unsigned i = 1; i < symnum; i++){
        uint16_t shndx = be16(SYM(i,14));
        uint32_t val = be32(SYM(i,4));
        const char *name = (const char *)(e + stroff + be32(SYM(i,0)));
        if(shndx == 0){                                    /* SHN_UNDEF -> hook */
            int found = -1;
            for(unsigned h = 0; h < N_HOOKS; h++)
                if(strcmp(name, hook_names[h]) == 0){ found = (int)h; break; }
            if(found < 0 && name[0]){
                fprintf(stderr, "m68k-gate: unresolved import %s\n", name); exit(97);
            }
            if(found >= 0) symaddr[i] = HOOK_BASE + (uint32_t)found * 16;
        } else if(shndx < shnum){
            symaddr[i] = secaddr[shndx] + val;
        }
        if(strcmp(name, "_chq_aud_run") == 0) gate_entry = symaddr[i];
    }
    if(!gate_entry){ fprintf(stderr, "m68k-gate: _chq_aud_run not found\n"); exit(97); }
    if(getenv("CHQ_GATESYMS")){          /* guest-addr -> symbol map for the profiler */
        FILE *sf = fopen(getenv("CHQ_GATESYMS"), "w");
        if(sf){
            for(unsigned i = 1; i < symnum; i++){
                const char *name = (const char *)(e + stroff + be32(SYM(i,0)));
                if(symaddr[i] && name[0]) fprintf(sf, "%08x %s\n", symaddr[i], name);
            }
            fclose(sf);
        }
    }

    /* pass 2: apply RELA (R_68K_32 only -- verified: the object contains nothing else) */
    unsigned long nrel = 0;
    for(unsigned i = 0; i < shnum; i++){
        if(be32(SH(i,4)) != 4 /*RELA*/) continue;
        unsigned tgt = be32(SH(i,0x1c));                   /* sh_info = target section */
        if(!secaddr[tgt]) continue;                        /* reloc for non-alloc section */
        uint32_t roff = be32(SH(i,0x10)), rsz = be32(SH(i,0x14));
        for(uint32_t r = 0; r < rsz; r += 12){
            const uint8_t *re = e + roff + r;
            uint32_t r_offset = be32(re), r_info = be32(re+4);
            int32_t  r_addend = (int32_t)be32(re+8);
            unsigned rtype = r_info & 0xff, rsym = r_info >> 8;
            if(rtype != 1){ fprintf(stderr, "m68k-gate: reloc type %u unsupported\n", rtype); exit(97); }
            wbe32(G + secaddr[tgt] + r_offset, symaddr[rsym] + (uint32_t)r_addend);
            nrel++;
        }
    }
    fprintf(stderr, "m68k-gate: loaded %s: image %#x..%#x entry %#x relocs %lu\n",
            path, CODE_BASE, cur, gate_entry, nrel);
    free(symaddr); free(secaddr); free(e);
}

static void gate_init(void){
    const char *obj = getenv("CHQ_M68KOBJ");
    if(!obj){ fprintf(stderr, "m68k-gate: set CHQ_M68KOBJ=<m68k cgu ELF object>\n"); exit(97); }
    gate_load_elf(obj);
    for(unsigned h = 0; h < N_HOOKS; h++) wbe16(G + HOOK_BASE + h*16, 0x4e75);  /* rts */
    wbe16(G + RET_MAGIC, 0x60fe);                                              /* bra . */
    wbe32(G + 0, STACK_TOP);      /* reset SSP */
    wbe32(G + 4, RET_MAGIC);      /* reset PC (parked) */
    g68k_m68k_init();
    g68k_m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    g68k_m68k_set_instr_hook_callback(gate_hook);
    g68k_m68k_pulse_reset();
    if(getenv("CHQ_GATEPROF")){
        prof_hist = calloc(G_SIZE >> 4, sizeof(unsigned));
        atexit(prof_dump);
    }
    gate_ready = 1;
}

/* the entry cc_audio.c calls in CHQ_AUDIO_NATIVE mode (in place of the Rust crate) */
void chq_aud_run(ChqZ80Native *s){
    if(!gate_ready) gate_init();
    host_mem = s->mem;
    uint8_t *st = G + STATE_ADDR;
    st[0]=s->b; st[1]=s->c; st[2]=s->d; st[3]=s->e; st[4]=s->h; st[5]=s->l; st[6]=s->a; st[7]=s->f;
    st[8]=s->ixh; st[9]=s->ixl; st[10]=s->iyh; st[11]=s->iyl;
    wbe16(st+12, s->sp); wbe16(st+14, s->pc);
    wbe16(st+16, s->bc_alt); wbe16(st+18, s->de_alt); wbe16(st+20, s->hl_alt); wbe16(st+22, s->af_alt);
    st[24]=s->i; st[25]=s->r; st[26]=s->iff1; st[27]=s->iff2;
    st[28]=s->im; st[29]=s->halted; st[30]=s->stop; st[31]=s->pad0;
    wbe32(st+32, s->cycles); wbe32(st+36, s->budget);
    wbe16(st+40, s->badpc); wbe16(st+42, s->pad1);
    wbe32(st+44, MEM_ADDR);
    memcpy(G + MEM_ADDR, s->mem, 0x10000);

    uint32_t sp = STACK_TOP - 8;
    wbe32(G + sp, RET_MAGIC);
    wbe32(G + sp + 4, STATE_ADDR);
    g68k_m68k_set_reg(M68K_REG_SP, sp);
    g68k_m68k_set_reg(M68K_REG_PC, gate_entry);
    gate_done = 0;
    gate_calls++;
    for(int spin = 0; !gate_done; spin++){
        gate_cycles += (unsigned long long)g68k_m68k_execute(20000000);
        if(spin > 200){
            fprintf(stderr, "m68k-gate: RUNAWAY guest: pc=%08x sp=%08x zpc=%04x cycles=%u budget=%u stop=%u\n",
                    g68k_m68k_get_reg(NULL, M68K_REG_PC), g68k_m68k_get_reg(NULL, M68K_REG_SP),
                    be16(st+14), be32(st+32), be32(st+36), st[30]);
            exit(96);
        }
    }

    s->b=st[0]; s->c=st[1]; s->d=st[2]; s->e=st[3]; s->h=st[4]; s->l=st[5]; s->a=st[6]; s->f=st[7];
    s->ixh=st[8]; s->ixl=st[9]; s->iyh=st[10]; s->iyl=st[11];
    s->sp=be16(st+12); s->pc=be16(st+14);
    s->bc_alt=be16(st+16); s->de_alt=be16(st+18); s->hl_alt=be16(st+20); s->af_alt=be16(st+22);
    s->i=st[24]; s->r=st[25]; s->iff1=st[26]; s->iff2=st[27];
    s->im=st[28]; s->halted=st[29]; s->stop=st[30]; s->pad0=st[31];
    s->cycles=be32(st+32); s->budget=be32(st+36);
    s->badpc=be16(st+40); s->pad1=be16(st+42);
    memcpy(s->mem, G + MEM_ADDR, 0x10000);
    host_mem = NULL;
}

/* cc_hosttest prints this in CHQ_AUDIO_NATIVE mode; the m68k crate build has no
 * host-feature counter, so report the emulated-68k workload instead (harmless). */
unsigned long long chq_aud_ione_hits(void){ return gate_cycles / (gate_calls ? gate_calls : 1); }
