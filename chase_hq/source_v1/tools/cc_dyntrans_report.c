/* cc_dyntrans_report.c -- Chase HQ coverage audit for the 68000 dyntrans path.
 * Reads cc_hostcoverage output and checks the executed PCs against the shared
 * Shinobi 68000 decoder/emitter. This is a host-side planning tool only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "shinobi_xlate.h"
#include "cc_xlate_plan.h"
#include "m68k.h"

static uint8_t rom_main[0x80000];
static uint8_t rom_sub[0x20000];

unsigned int m68k_read_disassembler_8(unsigned int address){ (void)address; return 0xff; }
unsigned int m68k_read_disassembler_16(unsigned int address){ (void)address; return 0xffff; }
unsigned int m68k_read_disassembler_32(unsigned int address){ (void)address; return 0xffffffffu; }

static void load_file(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fread(dst, 1, n, f) != n) { fprintf(stderr, "short read: %s\n", path); exit(1); }
    fclose(f);
}

static unsigned rd16(const uint8_t *g, uint32_t gsize, uint32_t a)
{
    if (a + 1 >= gsize) return 0xffff;
    return ((unsigned)g[a] << 8) | g[a + 1];
}

struct stats {
    unsigned pcs;
    unsigned body;
    unsigned term[XT_N];
    unsigned emit_ok;
    unsigned emit_bad;
    unsigned unclassified;
    unsigned abs_refs;
    unsigned abs_region[16];
    unsigned helper_refs;
    unsigned helper_instrs;
    unsigned direct_helper_refs;
    unsigned pointer_helper_refs;
    unsigned pointer_loads;
    unsigned ptr_trace_count;
    struct { char cpu[8]; uint32_t pc; uint32_t target; unsigned areg; unsigned region; } ptr_trace[32];
    unsigned first_abs_count;
    struct { char cpu[8]; uint32_t pc; uint32_t target; uint8_t kind; char dis[96]; } first_abs[64];
    unsigned hw_pat_count;
    struct { char dis[96]; unsigned region; unsigned count; } hw_pat[256];
    unsigned first_bad_count;
    struct { char cpu[8]; uint32_t pc; uint16_t op; int term; uint32_t len; } first_bad[64];
};

static void note_hw_pattern(struct stats *s, unsigned region, const char *dis)
{
    for (unsigned i = 0; i < s->hw_pat_count; i++) {
        if (s->hw_pat[i].region == region && strcmp(s->hw_pat[i].dis, dis) == 0) {
            s->hw_pat[i].count++;
            return;
        }
    }
    if (s->hw_pat_count < (sizeof s->hw_pat / sizeof s->hw_pat[0])) {
        unsigned i = s->hw_pat_count++;
        snprintf(s->hw_pat[i].dis, sizeof s->hw_pat[i].dis, "%s", dis);
        s->hw_pat[i].region = region;
        s->hw_pat[i].count = 1;
    }
}

static void note_bad(struct stats *s, const char *cpu, uint32_t pc, uint16_t op, int term, uint32_t len)
{
    s->emit_bad++;
    if (term == XT_UNCL) s->unclassified++;
    if (s->first_bad_count < (sizeof s->first_bad / sizeof s->first_bad[0])) {
        unsigned i = s->first_bad_count++;
        snprintf(s->first_bad[i].cpu, sizeof s->first_bad[i].cpu, "%s", cpu);
        s->first_bad[i].pc = pc;
        s->first_bad[i].op = op;
        s->first_bad[i].term = term;
        s->first_bad[i].len = len;
    }
}

static void check_pc(struct stats *s, const char *cpu, const uint8_t *rom, uint32_t rom_size, uint32_t pc)
{
    xdec d;
    uint8_t out[256];
    xl_emit_env env;
    int emitted;
    memset(&env, 0, sizeof env);
    env.base = 0x10000000u;
    env.gregs_pc = 0x20000000u;
    env.exit_thunk = 0x20000010u;
    env.gregs_sr = 0x20000020u;
    env.gregs_ccr = 0x20000024u;
    env.fault_pc = 0x20000028u;
    env.fault_sentinel = 0xdeadfa11u;

    if (pc >= rom_size) return;
    xl_decode(rom, rom_size, pc, &d);
    s->pcs++;
    s->abs_refs += (unsigned)d.nabs;
    int instr_needs_helper = 0;
    for (int i = 0; i < d.nabs; i++) {
        uint32_t t = ccxl_abs_target(rom, rom_size, pc, &d, i);
        unsigned r = d.abs[i].kind == XF_PCREL16 ? CCXL_REG_PCREL_DATA : ccxl_region_for_addr(t);
        unsigned areg = 0;
        int is_ptr_load = ccxl_is_addr_reg_load(d.w0, &d, i, &areg);
        if (r < (sizeof s->abs_region / sizeof s->abs_region[0])) s->abs_region[r]++;
        if (ccxl_region_needs_helper(r)) {
            s->helper_refs++;
            instr_needs_helper = 1;
            if (is_ptr_load) {
                s->pointer_helper_refs++;
                s->pointer_loads++;
                if (s->ptr_trace_count < (sizeof s->ptr_trace / sizeof s->ptr_trace[0])) {
                    unsigned n = s->ptr_trace_count++;
                    snprintf(s->ptr_trace[n].cpu, sizeof s->ptr_trace[n].cpu, "%s", cpu);
                    s->ptr_trace[n].pc = pc;
                    s->ptr_trace[n].target = t;
                    s->ptr_trace[n].areg = areg;
                    s->ptr_trace[n].region = r;
                }
            } else {
                s->direct_helper_refs++;
            }
        }
        if (r >= 4 && r <= 11) {
            char dis[96];
            m68k_disassemble_raw(dis, pc, rom + pc, rom + pc + 2, M68K_CPU_TYPE_68000);
            note_hw_pattern(s, r, dis);
        }
        if (r != 0 && r != 15 && s->first_abs_count < (sizeof s->first_abs / sizeof s->first_abs[0])) {
            unsigned n = s->first_abs_count++;
            snprintf(s->first_abs[n].cpu, sizeof s->first_abs[n].cpu, "%s", cpu);
            s->first_abs[n].pc = pc;
            s->first_abs[n].target = t;
            s->first_abs[n].kind = d.abs[i].kind;
            m68k_disassemble_raw(s->first_abs[n].dis, pc, rom + pc, rom + pc + 2, M68K_CPU_TYPE_68000);
        }
    }
    if (instr_needs_helper) s->helper_instrs++;
    if (d.term >= 0 && d.term < XT_N) s->term[d.term]++;
    if (d.term == XT_NONE) {
        s->body++;
        emitted = xl_emit_instr(rom, rom_size, pc, &d, &env, out, sizeof out);
    } else {
        emitted = xl_emit_term(pc, &d, pc + d.len, &env, out, sizeof out);
    }
    if (emitted > 0) s->emit_ok++;
    else note_bad(s, cpu, pc, (uint16_t)rd16(rom, rom_size, pc), d.term, d.len);
}

static void print_pointer_followup(const uint8_t *rom, uint32_t rom_size, uint32_t pc,
                                   unsigned areg, unsigned region)
{
    uint32_t a = pc;
    printf("  pc=%06x A%u=%s follow-up:\n", pc, areg, ccxl_region_name(region));
    for (unsigned n = 0; n < 12 && a < rom_size; n++) {
        xdec d;
        char dis[96];
        xl_decode(rom, rom_size, a, &d);
        m68k_disassemble_raw(dis, a, rom + a, rom + a + 2, M68K_CPU_TYPE_68000);
        printf("    %06x  %s\n", a, dis);
        a += d.len ? d.len : 2;
        if (d.term != XT_NONE) break;
    }
}

static void report_stats(const struct stats *s)
{
    printf("pcs=%u body=%u emit_ok=%u emit_bad=%u unclassified=%u abs_refs=%u helper_refs=%u helper_instrs=%u direct_helper_refs=%u pointer_helper_refs=%u pointer_loads=%u\n",
           s->pcs, s->body, s->emit_ok, s->emit_bad, s->unclassified,
           s->abs_refs, s->helper_refs, s->helper_instrs,
           s->direct_helper_refs, s->pointer_helper_refs, s->pointer_loads);
    for (int i = 0; i < XT_N; i++) {
        if (s->term[i]) printf("term %-14s %u\n", xt_name[i], s->term[i]);
    }
    for (unsigned i = 0; i < (sizeof s->abs_region / sizeof s->abs_region[0]); i++) {
        if (s->abs_region[i]) printf("abs %-14s %u\n", ccxl_region_name(i), s->abs_region[i]);
    }
    if (s->first_abs_count) {
        printf("first non-ROM absolute refs:\n");
        for (unsigned i = 0; i < s->first_abs_count; i++) {
            printf("  %s pc=%06x target=%06x kind=%u region=%s  %s\n",
                   s->first_abs[i].cpu, s->first_abs[i].pc, s->first_abs[i].target,
                   s->first_abs[i].kind, ccxl_region_name(ccxl_region_for_addr(s->first_abs[i].target)),
                   s->first_abs[i].dis);
        }
    }
    if (s->hw_pat_count) {
        printf("hardware-port patterns:\n");
        for (unsigned i = 0; i < s->hw_pat_count; i++) {
            printf("  %-14s x%-3u %s\n",
                   ccxl_region_name(s->hw_pat[i].region), s->hw_pat[i].count, s->hw_pat[i].dis);
        }
    }
    if (s->ptr_trace_count) {
        printf("hardware pointer load follow-ups:\n");
        for (unsigned i = 0; i < s->ptr_trace_count; i++) {
            const uint8_t *rom = strcmp(s->ptr_trace[i].cpu, "sub") == 0 ? rom_sub : rom_main;
            uint32_t size = strcmp(s->ptr_trace[i].cpu, "sub") == 0 ? sizeof rom_sub : sizeof rom_main;
            printf("%s ", s->ptr_trace[i].cpu);
            print_pointer_followup(rom, size, s->ptr_trace[i].pc,
                                   s->ptr_trace[i].areg, s->ptr_trace[i].region);
        }
    }
    if (s->first_bad_count) {
        printf("first unsupported/failed PCs:\n");
        for (unsigned i = 0; i < s->first_bad_count; i++) {
            printf("  %s pc=%06x op=%04x term=%s len=%u\n",
                   s->first_bad[i].cpu, s->first_bad[i].pc, s->first_bad[i].op,
                   (s->first_bad[i].term >= 0 && s->first_bad[i].term < XT_N) ? xt_name[s->first_bad[i].term] : "?",
                   s->first_bad[i].len);
        }
    }
}

int main(int argc, char **argv)
{
    const char *coverage = argc > 1 ? argv[1] : "/tmp/chq_pc_coverage.txt";
    char line[128];
    char section = 0;
    struct stats st;
    FILE *f;
    memset(&st, 0, sizeof st);
    load_file("data/maincpu.bin", rom_main, sizeof rom_main);
    load_file("data/sub.bin", rom_sub, sizeof rom_sub);
    f = fopen(coverage, "r");
    if (!f) { perror(coverage); return 1; }
    while (fgets(line, sizeof line, f)) {
        char *end;
        unsigned long pc;
        if (line[0] == '#') continue;
        if (strncmp(line, "[main]", 6) == 0) { section = 'm'; continue; }
        if (strncmp(line, "[sub]", 5) == 0) { section = 's'; continue; }
        pc = strtoul(line, &end, 16);
        if (end == line) continue;
        if (section == 'm') check_pc(&st, "main", rom_main, sizeof rom_main, (uint32_t)pc);
        else if (section == 's') check_pc(&st, "sub", rom_sub, sizeof rom_sub, (uint32_t)pc);
    }
    fclose(f);
    report_stats(&st);
    return st.emit_bad ? 2 : 0;
}
