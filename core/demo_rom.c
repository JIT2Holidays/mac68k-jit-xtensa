/* Built-in 68000 demo program. See demo_rom.h.
 *
 * Assembled with the header-only m68k_asm.h. The image carries its own
 * reset vector at offset 0/4 so it can be loaded straight at address 0. */

#include "demo_rom.h"
#include "mac_mem.h"
#include "m68k_asm.h"

#define DEMO_SSP   0x00008000u   /* initial supervisor stack pointer */
#define DEMO_ENTRY 0x00000100u   /* program start (past the vector area) */
#define DBG_SERIAL (MAC_DEBUG_BASE + MAC_DBG_SERIAL)
#define DBG_EXIT   (MAC_DEBUG_BASE + MAC_DBG_EXIT)

u32 demo_rom_build(u8 *out, u32 fb_addr) {
    m68a a;
    m68a_init(&a, out, DEMO_ROM_MAX, 0);

    /* Reset vector: SSP at 0, PC at 4. */
    m68a_w32(&a, DEMO_SSP);
    m68a_w32(&a, DEMO_ENTRY);
    while (m68a_here(&a) < DEMO_ENTRY) m68a_w16(&a, 0x4E71);  /* pad with NOP */

    int l_sumloop = m68a_label(&a);
    int l_fbloop  = m68a_label(&a);
    int l_prloop  = m68a_label(&a);
    int l_prdone  = m68a_label(&a);
    int l_flloop  = m68a_label(&a);
    int l_fldone  = m68a_label(&a);
    int l_dbl     = m68a_label(&a);
    int l_fail    = m68a_label(&a);
    int l_msg     = m68a_label(&a);
    int l_failmsg = m68a_label(&a);

    /* sum := 1 + 2 + ... + 100  (expect 5050) */
    m68a_nop(&a);
    m68a_moveq(&a, 0, 0);                 /* D0 = 0    (inline JIT path) */
    m68a_move_l_imm(&a, 1, 100);          /* D1 = 100  */
    m68a_mark(&a, l_sumloop);
    m68a_add_l(&a, 0, 1);                 /* D0 += D1  */
    m68a_subq_l(&a, 1, 1);                /* D1 -= 1   */
    m68a_bne(&a, l_sumloop);
    m68a_move_l_imm(&a, 2, 5050);
    m68a_cmp_l(&a, 0, 2);                 /* D0 == 5050 ? */
    m68a_bne(&a, l_fail);

    /* shift + logic check */
    m68a_move_l_imm(&a, 3, 0x12345678u);
    m68a_lsr_l(&a, 4, 3);                 /* D3 = 0x01234567 */
    m68a_asl_l(&a, 4, 3);                 /* D3 = 0x12345670 */
    m68a_move_l_imm(&a, 4, 0x0F0F0F0Fu);
    m68a_and_l(&a, 3, 4);                 /* D3 &= 0x0F0F0F0F */
    m68a_move_l_imm(&a, 5, 0x02040600u);  /* 0x12345670 & 0x0F0F0F0F */
    m68a_cmp_l(&a, 3, 5);
    m68a_bne(&a, l_fail);

    /* subroutine: dbl() doubles D6 */
    m68a_move_l_imm(&a, 6, 21);
    m68a_bsr(&a, l_dbl);                  /* D6 -> 42 */
    m68a_move_l_imm(&a, 7, 42);
    m68a_cmp_l(&a, 6, 7);
    m68a_bne(&a, l_fail);

    /* framebuffer fill: 128 longwords of 0xA5A5A5A5 */
    m68a_movea_l_imm(&a, 2, fb_addr);
    m68a_move_l_imm(&a, 6, 128);
    m68a_move_l_imm(&a, 7, 0xA5A5A5A5u);
    m68a_mark(&a, l_fbloop);
    m68a_move_l_to_an(&a, 2, 7);          /* (A2) = D7  */
    m68a_addq_w_an(&a, 4, 2);             /* A2 += 4    */
    m68a_subq_l(&a, 1, 6);                /* D6 -= 1    */
    m68a_bne(&a, l_fbloop);

    /* verify the first framebuffer longword */
    m68a_movea_l_imm(&a, 2, fb_addr);
    m68a_move_l_from_an(&a, 0, 2);        /* D0 = (A2)  */
    m68a_cmp_l(&a, 0, 7);                 /* D0 == 0xA5A5A5A5 ? */
    m68a_bne(&a, l_fail);

    /* print the success line over the debug serial port */
    m68a_lea_label(&a, 0, l_msg);
    m68a_movea_l_imm(&a, 1, DBG_SERIAL);
    m68a_mark(&a, l_prloop);
    m68a_move_b_postinc(&a, 0, 0);        /* D0 = (A0)+ */
    m68a_beq(&a, l_prdone);
    m68a_move_b_to_an(&a, 1, 0);          /* (A1) = D0  */
    m68a_bra(&a, l_prloop);
    m68a_mark(&a, l_prdone);
    m68a_moveq(&a, 0, 0);                 /* exit code 0 */
    m68a_movea_l_imm(&a, 1, DBG_EXIT);
    m68a_move_b_to_an(&a, 1, 0);          /* write exit port -> halt */
    m68a_stop(&a, 0x2700);

    /* dbl: D6 += D6 ; RTS */
    m68a_mark(&a, l_dbl);
    m68a_add_l(&a, 6, 6);
    m68a_rts(&a);

    /* fail path */
    m68a_mark(&a, l_fail);
    m68a_lea_label(&a, 0, l_failmsg);
    m68a_movea_l_imm(&a, 1, DBG_SERIAL);
    m68a_mark(&a, l_flloop);
    m68a_move_b_postinc(&a, 0, 0);
    m68a_beq(&a, l_fldone);
    m68a_move_b_to_an(&a, 1, 0);
    m68a_bra(&a, l_flloop);
    m68a_mark(&a, l_fldone);
    m68a_moveq(&a, 1, 1);                 /* exit code 1 */
    m68a_movea_l_imm(&a, 1, DBG_EXIT);
    m68a_move_b_to_an(&a, 1, 1);
    m68a_stop(&a, 0x2700);

    /* data */
    m68a_mark(&a, l_msg);
    m68a_string(&a, "RESULT: PASS\n");
    m68a_mark(&a, l_failmsg);
    m68a_string(&a, "RESULT: FAIL\n");

    m68a_finish(&a);
    return a.len;
}
