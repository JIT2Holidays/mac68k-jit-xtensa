/* minem_glue.c — wraps mini vMac's 68000 core (MINEM68K.c) so the
 * differential harness can drive it one instruction at a time.
 *
 * This file #includes MINEM68K.c so the helpers below share its
 * translation unit and can reach its static `regs` and local
 * procedures. Compiled with the mini vMac config headers on the include
 * path (see tools/build_mvmac_diff.sh). */

/* Rename mini vMac's exported CPU entry points that would otherwise
 * clash with this project's own m68k core at link time. */
#define m68k_reset       mvm_m68k_reset
#define m68k_go_nCycles  mvm_m68k_go_nCycles

#include "MINEM68K.c"

/* --- address-translation table — RAM / ROM direct, rest via MMDV ------ */

static ATTer g_att[3];
static unsigned char g_ipl;          /* interrupt priority level (0) */

void MNVM_init(unsigned char *ramB, unsigned rammask,
               unsigned char *romB, unsigned rommask) {
    g_ipl = 0;
    MINEM68K_Init(&g_ipl);

    g_att[0].Next = &g_att[1];                  /* RAM 0x000000-0x3FFFFF */
    g_att[0].cmpmask = 0xC00000; g_att[0].cmpvalu = 0x000000;
    g_att[0].Access  = kATTA_readwritereadymask;
    g_att[0].usemask = rammask;  g_att[0].usebase = ramB;
    g_att[0].MMDV = 0; g_att[0].Ntfy = 0;

    g_att[1].Next = &g_att[2];                  /* ROM 0x400000-0x4FFFFF */
    g_att[1].cmpmask = 0xF00000; g_att[1].cmpvalu = 0x400000;
    g_att[1].Access  = kATTA_readreadymask;
    g_att[1].usemask = rommask;  g_att[1].usebase = romB;
    g_att[1].MMDV = 0; g_att[1].Ntfy = 0;

    g_att[2].Next = (ATTep)0;                   /* end guard — everything */
    g_att[2].cmpmask = 0; g_att[2].cmpvalu = 0;
    g_att[2].Access  = kATTA_mmdvmask;
    g_att[2].usemask = 0; g_att[2].usebase = (ui3p)0;
    g_att[2].MMDV = 0; g_att[2].Ntfy = 0;

    SetHeadATTel(&g_att[0]);
}

/* Load a full CPU state (16 regs, pc, sr, both stack pointers). */
void MNVM_set_state(const unsigned r[16], unsigned pc, unsigned sr,
                    unsigned usp, unsigned ssp) {
    int i;
    for (i = 0; i < 16; i++) regs.regs[i] = r[i];
    regs.s   = (sr >> 13) & 1;        /* match new S so setSR won't swap */
    regs.usp = usp;
    regs.isp = ssp;
    m68k_setSR((ui4rr)sr);            /* materialise flags + intmask     */
    regs.pc = 0;                      /* force a PC-block recalculation  */
    regs.pc_pLo = (ui3p)0;
    regs.pc_pHi = (ui3p)0;
    m68k_setpc((CPTR)pc);
}

void MNVM_get_regs(unsigned out[16]) {
    int i;
    for (i = 0; i < 16; i++) out[i] = (unsigned)(regs.regs[i] & 0xFFFFFFFFu);
}
unsigned MNVM_getpc(void) { return (unsigned)m68k_getpc(); }
unsigned MNVM_getSR(void) { return (unsigned)m68k_getSR(); }

/* Execute exactly one instruction. */
void MNVM_step(void) {
    V_MaxCyclesToGo = 1;
    m68k_go_MaxCycles();
}

/* --- externals that MINEM68K.c expects -------------------------------- */

/* The harness replays this project's own device-read values here, so
 * both CPUs see identical memory-mapped I/O. */
extern unsigned mvdiff_dev_read(unsigned addr, int is_word);

ui5b MMDV_Access(ATTep p, ui5b Data, blnr WriteMem, blnr ByteSize,
                 CPTR addr) {
    (void)p;
    if (WriteMem) return Data;        /* device writes dropped (replayed) */
    return mvdiff_dev_read((unsigned)addr, ByteSize ? 0 : 1);
}
blnr MemAccessNtfy(ATTep pT) { (void)pT; return falseblnr; }
void customreset(void) {}
