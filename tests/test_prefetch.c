/* Unit tests for the M6.71 static-successor analysis used by the JIT
 * dispatcher's block-prefetch policy. Hand-builds blocks with various
 * terminators and checks m68k_block_static_successors() returns the
 * correct PCs.
 *
 * Run via ctest (added in tests/CMakeLists.txt).
 */
#include "codegen.h"
#include "mac_mem.h"
#include <stdio.h>
#include <string.h>

static int g_fail;

static const char *current_test;

static void expect_eq(const char *what, u32 want, u32 got) {
    if (want != got) {
        fprintf(stderr, "FAIL %s: %s want=%08X got=%08X\n",
                current_test, what, want, got);
        g_fail++;
    }
}

static void expect_eq_int(const char *what, int want, int got) {
    if (want != got) {
        fprintf(stderr, "FAIL %s: %s want=%d got=%d\n",
                current_test, what, want, got);
        g_fail++;
    }
}

/* Build a stub block with just the fields the helper reads. */
static void make_block(m68k_block *b, u16 last_op, u32 last_op_pc, u32 pc_end) {
    memset(b, 0, sizeof(*b));
    b->last_op = last_op;
    b->last_op_pc = last_op_pc;
    b->pc_end = pc_end;
}

int main(void) {
    mac_mem mem;
    mac_mem_init(&mem, 1024 * 1024);
    /* Turn the boot overlay off so RAM appears at low addresses (where
     * we write the test fixtures). The overlay defaults on at reset to
     * mirror the ROM; clearing it is what the boot ROM does early. */
    mem.overlay = false;

    /* Pre-load a few words into RAM for the helper to read disp/abs ops
     * from. Use the .L variants for ops that read extension words. */
    /* For JMP (xxx).L at pc=0x1000: target stored at 0x1002 as BE u32. */
    mem.ram[0x1002] = 0x12; mem.ram[0x1003] = 0x34;
    mem.ram[0x1004] = 0x56; mem.ram[0x1005] = 0x78;
    /* For BRA.W at pc=0x2000: disp16 at 0x2002 = 0x0100  → target 0x2102 */
    mem.ram[0x2002] = 0x01; mem.ram[0x2003] = 0x00;
    /* For DBcc at pc=0x3000: disp16 at 0x3002 = -8 (0xFFF8)
     * → taken = 0x3000+2-8 = 0x2FFA, ft = 0x3004 */
    mem.ram[0x3002] = 0xFF; mem.ram[0x3003] = 0xF8;
    /* For JMP (d16,PC) at pc=0x4000: disp16 = 0x0040 → target 0x4042 */
    mem.ram[0x4002] = 0x00; mem.ram[0x4003] = 0x40;
    /* For Bcc.W at pc=0x5000: disp16 = 0x0080 → taken 0x5082, ft 0x5004 */
    mem.ram[0x5002] = 0x00; mem.ram[0x5003] = 0x80;

    m68k_block b;
    u32 out[2];
    int n;

    current_test = "BRA.S +0x40";
    /* 0x6040 = BRA.S with disp 0x40 → target = op_pc + 2 + 0x40 */
    make_block(&b, 0x6040, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x100 + 2 + 0x40, out[0]);

    current_test = "BRA.W disp16=0x0100";
    /* 0x6000 = BRA.W (disp byte 0); disp16 in next word */
    make_block(&b, 0x6000, 0x2000, 0x2004);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x2000 + 2 + 0x100, out[0]);

    current_test = "BRA.L (treated as dynamic)";
    /* 0x60FF = BRA.L (disp byte 0xFF) — 68020+, returned as dynamic. */
    make_block(&b, 0x60FF, 0x100, 0x106);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "BEQ.S +0x20";
    /* 0x6720 = BEQ.S with disp 0x20 → taken = op_pc+2+0x20, ft = op_pc+2 */
    make_block(&b, 0x6720, 0x200, 0x202);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 2, n);
    expect_eq("taken", 0x200 + 2 + 0x20, out[0]);
    expect_eq("fall-through", 0x200 + 2, out[1]);

    current_test = "BEQ.W disp16=0x0080";
    /* 0x6700 = BEQ.W → taken+ft */
    make_block(&b, 0x6700, 0x5000, 0x5004);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 2, n);
    expect_eq("taken", 0x5000 + 2 + 0x80, out[0]);
    expect_eq("fall-through", 0x5000 + 4, out[1]);

    current_test = "BSR.S +0x10";
    /* 0x6110 = BSR.S — RTS dynamic but CALL target static. cc=1, disp=0x10 */
    make_block(&b, 0x6110, 0x300, 0x302);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x300 + 2 + 0x10, out[0]);

    current_test = "DBNE.W disp16=-8";
    /* 0x56C8 = DBNE D0 — top=5, bits 6-7 = 3, bits 3-5 = 1.
     * 0101 0110 1100 1000.
     *  bits 11-9 = cc=011 (NE? let me check)
     *  bits 8-6  = 011 (size field for DBcc = 3)
     *  bits 5-3  = 001 (mode 1 = An / Dn-cap, for DBcc means Dn)
     *  bits 2-0  = 000 (Dn = D0)
     * Actually the DBcc test bits are bits 11-8 (cc), but encoding is
     * 0101 cccc 11 001 ddd. We check the helper's bit pattern match. */
    make_block(&b, 0x56C8, 0x3000, 0x3004);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 2, n);
    expect_eq("fall-through", 0x3004, out[0]);
    /* disp = -8 → taken = 0x3002 - 8 = 0x2FFA */
    expect_eq("taken", 0x3000 + 2 - 8, out[1]);

    current_test = "JMP (xxx).L";
    /* 0x4EF9 with target 0x12345678 at op_pc+2 */
    make_block(&b, 0x4EF9, 0x1000, 0x1006);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x12345678u, out[0]);

    current_test = "JMP (d16,PC)";
    /* 0x4EFA — d16 = 0x40 → target = 0x4002 + 0x40 = 0x4042 */
    make_block(&b, 0x4EFA, 0x4000, 0x4004);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x4002u + 0x40u, out[0]);

    current_test = "JSR (xxx).L";
    /* 0x4EB9 with target 0x12345678 at op_pc+2 — same memory as JMP test */
    make_block(&b, 0x4EB9, 0x1000, 0x1006);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("target", 0x12345678u, out[0]);

    current_test = "RTS (dynamic)";
    make_block(&b, 0x4E75, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "RTE (dynamic)";
    make_block(&b, 0x4E73, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "TRAP #5 (dynamic)";
    /* 0x4E40..0x4E4F = TRAP #n */
    make_block(&b, 0x4E45, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "JMP (An) (dynamic)";
    /* 0x4ED0..0x4ED7 = JMP (An), mode 2 / reg n. */
    make_block(&b, 0x4ED1, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "line-A trap (dynamic)";
    /* Top nibble 0xA = line-A. */
    make_block(&b, 0xA123, 0x100, 0x102);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "STOP (dynamic)";
    make_block(&b, 0x4E72, 0x100, 0x104);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 0, n);

    current_test = "block-size-cap fall-through (non-control-flow last op)";
    /* MOVE.W Dm,Dn (0x3200) — not a terminator. JIT block walker would
     * have hit n_ops cap; pc_end = where the next op would start. */
    make_block(&b, 0x3200, 0x6000, 0x6002);
    n = m68k_block_static_successors(&b, &mem, out);
    expect_eq_int("count", 1, n);
    expect_eq("fall-through", 0x6002, out[0]);

    mac_mem_free(&mem);

    if (g_fail) { printf("FAIL: %d prefetch unit tests failed\n", g_fail); return 1; }
    printf("PASS: prefetch unit tests (static_successors)\n");
    return 0;
}
