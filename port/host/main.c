/* Host CLI driver for the mac68k-jit-xtensa emulator.
 *
 * Runs a 68000 program either under the reference interpreter or the JIT
 * (the latter executing through the in-tree Xtensa simulator, since the
 * host is not an Xtensa). With no ROM path it runs the built-in demo. */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "mac_input.h"
#include "mac_snd.h"
#include "sony.h"
#include "demo_rom.h"
#include "dispatcher.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

/* Write the 512x342 1bpp Mac framebuffer as an 8-bit grayscale BMP so the
 * boot screen can be inspected. Mac convention: a set bit is a black pixel. */
static void write_screen_bmp(const mac_mem *m, const char *path) {
    int W = MAC_SCREEN_W, H = MAC_SCREEN_H;
    int row = (W + 3) & ~3, isz = row * H;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(int *)(hdr + 2) = 54 + 1024 + isz;
    *(int *)(hdr + 10) = 54 + 1024;
    *(int *)(hdr + 14) = 40;
    *(int *)(hdr + 18) = W; *(int *)(hdr + 22) = H;
    hdr[26] = 1; hdr[28] = 8;
    *(int *)(hdr + 34) = isz;
    fwrite(hdr, 1, 54, f);
    for (int i = 0; i < 256; i++) {
        unsigned char pe[4] = { (unsigned char)i, (unsigned char)i,
                                (unsigned char)i, 0 };
        fwrite(pe, 1, 4, f);
    }
    unsigned char *ln = (unsigned char *)calloc(1, (size_t)row);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            u32 a = m->fb_base + (u32)y * (W / 8) + (u32)(x >> 3);
            u8 byte = m->ram[a % m->ram_size];
            ln[x] = ((byte >> (7 - (x & 7))) & 1) ? 0 : 255;
        }
        fwrite(ln, 1, (size_t)row, f);
    }
    free(ln);
    fclose(f);
}

/* --- GUI server mode --------------------------------------------------
 * Runs the Mac and speaks the GUI wire protocol (gui/protocol.h) over
 * stdin/stdout: framebuffer packets out, mouse/key packets in. The SDL
 * GUI spawns this with pipes. */

static double mono_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Write the whole buffer, looping over partial writes — a short write
 * here would desync the protocol stream permanently. */
static int write_all(const u8 *buf, u32 len) {
    u32 off = 0;
    while (off < len) {
        ssize_t n = write(1, buf + off, len - off);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        off += (u32)n;
    }
    return 0;
}

static void send_packet(u8 type, const u8 *payload, u32 len) {
    u8 hdr[4] = { type, (u8)len, (u8)(len >> 8), (u8)(len >> 16) };
    if (write_all(hdr, 4) != 0) return;
    if (len) write_all(payload, len);
}

/* Server pacing multiplier: real-Mac speed × g_speed_mult / 100.
 *   100 = 1× (7.83 MHz, the default — Mac Plus original clock)
 *   200 = 2×, 400 = 4×, 800 = 8×
 *     0 = uncapped (let the JIT/interp run as fast as the host can)
 * Set via the GUI's Speed menu button, which sends MACGUI_PKT_SPEED. */
static u32 g_speed_mult = 100;

static void server_apply_packet(mac_mem *m, u8 type, const u8 *p, u32 len) {
    if (type == MACGUI_PKT_MOUSE && len >= 5) {
        i16 x = (i16)(p[0] | (p[1] << 8));
        i16 y = (i16)(p[2] | (p[3] << 8));
        mac_set_mouse(m, x, y, p[4] != 0);
    } else if (type == MACGUI_PKT_KEY && len >= 2) {
        mac_key_event(m, p[0], p[1] != 0);
    } else if (type == MACGUI_PKT_SPEED && len >= 2) {
        (void)m;
        g_speed_mult = (u32)(p[0] | (p[1] << 8));
        fprintf(stderr, "[server] speed multiplier = %u (%.2f× real Mac)\n",
                g_speed_mult, g_speed_mult / 100.0);
    }
}

/* --- scripted debug run ----------------------------------------------
 * Boots the Mac unpaced (fast) and replays a fixed mouse script that
 * opens the Apple-menu Control Panel, dumping the framebuffer along the
 * way — to reproduce the Control Panel rendering bug deterministically.
 * Enabled with the MAC68K_CPDEBUG env var. */

/* kind: 0 = mouse (x,y,btn used); 1 = key (x=keycode, btn=down). */
typedef struct { u64 cyc; int kind; int x, y, btn; } script_ev;

/* Write-watch: while armed, tallies which PC writes into the two
 * corrupted framebuffer bands of the Control Panel. */
static struct { u32 pc, n; } g_wpc[512];
static int  g_wpc_n;
static bool g_watch_armed;

static void cp_write_watch(void *ctx, u32 addr) {
    if (!g_watch_armed) return;
    mac_mem *m = (mac_mem *)ctx;
    u32 fb = m->fb_base;
    if (addr < fb) return;
    u32 row = (addr - fb) / 64;
    /* the two garbage bands (Mac screen rows) */
    if (!((row >= 138 && row <= 168) || (row >= 200 && row <= 235))) return;
    m68k_cpu *c = m->cpu;
    u32 pc = c->pc;
    /* dump full register state for the first writes by the dominant
     * band-writer (PC ~0x40AC3C) */
    static int dumped;
    if (dumped < 4 && (pc >= 0x40AC00 && pc <= 0x40AC80)) {
        dumped++;
        fprintf(stderr, "[bw] cyc=%llu pc=%06X addr=%06X row=%u A3=%08X\n",
                (unsigned long long)c->cycles, pc, addr, row, c->a[3]);
    }
    for (int i = 0; i < g_wpc_n; i++)
        if (g_wpc[i].pc == pc) { g_wpc[i].n++; return; }
    if (g_wpc_n < 512) { g_wpc[g_wpc_n].pc = pc; g_wpc[g_wpc_n].n = 1;
                         g_wpc_n++; }
}

/* Log _CopyBits calls — the QuickDraw blit (trap 0xA8EC). The corrupt
 * bands are 68k code drawn as a bitmap, so a CopyBits is sourcing from a
 * bad address; this prints each call's source bitmap and dest rect. */
static void cp_trap_hook(m68k_cpu *cpu, u16 trap) {
    if (!g_watch_armed || trap != 0xA8EC) return;
    mac_mem *m = cpu->mem;
    u32 sp = cpu->a[7];
    u32 srcBits = mac_read32(m, sp + 18);
    u32 dstRect = mac_read32(m, sp + 6);
    u32 srcBase = mac_read32(m, srcBits);
    u16 srcRB   = mac_read16(m, srcBits + 4);
    int t = (i16)mac_read16(m, dstRect),     l = (i16)mac_read16(m, dstRect+2);
    int b = (i16)mac_read16(m, dstRect + 4), r = (i16)mac_read16(m, dstRect+6);
    /* Flag CopyBits whose source base address is not a sane 24-bit
     * pointer — those are the corrupting blits. */
    bool bad = (srcBase >> 24) != 0;
    fprintf(stderr, "[cp]%s CopyBits bitmap@%06X base=%08X rb=%u "
            "dst=(t%d l%d b%d r%d) caller-a6=%06X\n", bad ? " *BAD*" : "",
            srcBits, srcBase, srcRB, t, l, b, r, cpu->a[6]);
}

/* Dump a full machine snapshot (registers + RAM + patched ROM) for the
 * mini vMac differential harness. */
static void write_snapshot(mac_mem *m, m68k_cpu *cpu, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    u32 hdr[24];
    hdr[0] = 0x4D414331;                       /* 'MAC1' */
    for (int i = 0; i < 8; i++) hdr[1 + i]  = cpu->d[i];
    for (int i = 0; i < 8; i++) hdr[9 + i]  = cpu->a[i];
    hdr[17] = cpu->pc;
    hdr[18] = cpu->sr;
    hdr[19] = cpu->usp;
    hdr[20] = cpu->ssp;
    hdr[21] = hdr[22] = hdr[23] = 0;
    fwrite(hdr, 4, 24, f);
    fwrite(&m->ram_size, 4, 1, f);
    fwrite(m->ram, 1, m->ram_size, f);
    fwrite(&m->rom_size, 4, 1, f);
    fwrite(m->rom, 1, m->rom_size, f);
    fclose(f);
    fprintf(stderr, "[cpdebug] snapshot written: %s (pc=%06X cyc=%llu)\n",
            path, cpu->pc, (unsigned long long)cpu->cycles);
}

static int scripted_debug_run(mac_mem *m, m68k_cpu *cpu) {
    m->serial_sink = NULL;
    m68k_trap_hook = cp_trap_hook;
    const char *snap = getenv("MAC68K_SNAP");
    u64 snap_cyc = getenv("MAC68K_SNAP_CYCLE")
                 ? strtoull(getenv("MAC68K_SNAP_CYCLE"), NULL, 0) : 0;
    bool snapped = false;
    int item_y = getenv("MAC68K_CP_ITEMY") ? atoi(getenv("MAC68K_CP_ITEMY"))
                                           : 98;
    u64 boot = 900000000ull;
    script_ev script[] = {
        { boot,               0, 14, 10, 0 },
        { boot +  20000000,   0, 14, 10, 1 },  /* press — menu drops      */
        { boot +  60000000,   0, 30, 40, 1 },  /* drag down into the menu */
        { boot + 100000000,   0, 40, item_y, 1},/* onto Control Panel     */
        { boot + 150000000,   0, 40, item_y, 1},
        { boot + 170000000,   0, 40, item_y, 0},/* release — open it      */
    };
    int nev = (int)(sizeof(script) / sizeof(script[0])), next = 0;
    u64 end = boot + 600000000ull, last_dump = 0;
    int frame = 0;

    fprintf(stderr, "[cpdebug] running, item_y=%d\n", item_y);
    while (!cpu->halted && cpu->cycles < end) {
        m68k_run_until(cpu, cpu->cycles + 200000);
        if (snap && !snapped && cpu->cycles >= snap_cyc) {
            snapped = true;
            write_snapshot(m, cpu, snap);
        }
        while (next < nev && cpu->cycles >= script[next].cyc) {
            mac_set_mouse(m, script[next].x, script[next].y,
                          script[next].btn != 0);
            fprintf(stderr, "[cpdebug] ev%d cyc=%llu mouse=(%d,%d) btn=%d\n",
                    next, (unsigned long long)cpu->cycles, script[next].x,
                    script[next].y, script[next].btn);
            next++;
        }
        /* Arm the write-watch once the Control Panel starts opening. */
        if (!g_watch_armed && next >= nev) {
            g_watch_armed = true;
            mac_write_watch = cp_write_watch;
            mac_write_watch_ctx = m;
            fprintf(stderr, "[cpdebug] write-watch armed at %llu\n",
                    (unsigned long long)cpu->cycles);
        }
        if (cpu->cycles >= boot && cpu->cycles - last_dump >= 20000000ull) {
            last_dump = cpu->cycles;
            char path[64];
            snprintf(path, sizeof(path), "/tmp/cpf_%03d.bmp", frame++);
            write_screen_bmp(m, path);
        }
    }
    /* Report the PCs that wrote into the corrupted bands, most-frequent
     * first. */
    for (int i = 0; i < g_wpc_n; i++)
        for (int j = i + 1; j < g_wpc_n; j++)
            if (g_wpc[j].n > g_wpc[i].n) {
                u32 tp = g_wpc[i].pc, tn = g_wpc[i].n;
                g_wpc[i] = g_wpc[j];
                g_wpc[j].pc = tp; g_wpc[j].n = tn;
            }
    fprintf(stderr, "[cpdebug] band writers (top PCs):\n");
    for (int i = 0; i < g_wpc_n && i < 16; i++)
        fprintf(stderr, "  pc=%06X  writes=%u\n", g_wpc[i].pc, g_wpc[i].n);
    fprintf(stderr, "[cpdebug] done at cycle %llu\n",
            (unsigned long long)cpu->cycles);
    return 0;
}

/* --- generic mouse-script run ----------------------------------------
 * Boots the Mac unpaced and replays a mouse script loaded from the file
 * named by MAC68K_MOUSESCRIPT. Each non-blank, non-'#' line is
 *   <cycle> <x> <y> <btn>
 * Frames are dumped as BMPs into MAC68K_FRAMEDIR (default /tmp/spd) every
 * MAC68K_FRAME_EVERY cycles. Snapshots: if MAC68K_SNAP is set and
 * MAC68K_SNAP_EVERY is set, periodic <MAC68K_SNAP>.NNN snapshots are
 * written; otherwise a single snapshot at MAC68K_SNAP_CYCLE. The run
 * ends at MAC68K_END_CYCLE. This is the harness used to drive third-
 * party apps (e.g. Speedometer) for the mini vMac differential. */
static int read_file(const char *path, u8 **out, u32 *len);

/* --- drive-2 "hard disk" post-boot insert ----------------------------
 * The second disk (e.g. the Infinite HD) is inserted into drive 2 a
 * while after boot, not at reset: a disk present at reset mounts at the
 * .Sony level but the Finder — not yet running — never shows its icon.
 * Inserting it once the desktop is up delivers a real disk-inserted
 * event, exactly as if the user had connected an external drive. The
 * path is resolved in main(); g_hd_cycle is when to insert it.
 *
 * Default = 100M cycles ≈ 13 s of real time at the GUI's 7.83 MHz
 * pacing. The Finder is up by ~175M cycles in the headless path but
 * we insert sooner here — the Sony mount queue tolerates a pending
 * insert before the first floppy is mounted. Override with
 * MAC68K_DISK2_CYCLE if you need to insert later (e.g. when scripting
 * a slow-boot scenario). */
static const char *g_hd_path;
static u64  g_hd_cycle = 100000000ull;
static bool g_hd_inserted;

static void service_hd_insert(mac_mem *m, m68k_cpu *cpu) {
    if (!g_hd_path || g_hd_inserted || cpu->cycles < g_hd_cycle) return;
    g_hd_inserted = true;
    u8 *d = NULL; u32 dl = 0;
    if (read_file(g_hd_path, &d, &dl) == 0
        && mac_insert_disk_drive(m, 1, d, dl, false))
        fprintf(stderr, "[host] inserted %s into drive 2 (%u bytes)\n",
                g_hd_path, dl);
    else
        fprintf(stderr, "[host] failed to insert drive-2 disk %s\n",
                g_hd_path);
    free(d);
}

/* Trap-tracing hook installable per env. Logs every line-A Toolbox
 * trap word + PC + cycle within a [MAC68K_TRACE_FROM, MAC68K_TRACE_TO]
 * cycle window. Used to diagnose the M6.67 app-launch wall — see if
 * _Launch (0xA9F2) fires after a Cmd-O on a selected app. */
static u64 g_trace_from, g_trace_to;
static u32 g_trace_count;
static u32 g_trace_max = 200;     /* hard cap to keep log size sane */
static void trace_trap_hook(m68k_cpu *cpu, u16 trap) {
    if (cpu->cycles < g_trace_from || cpu->cycles > g_trace_to) return;
    if (g_trace_count >= g_trace_max) return;
    g_trace_count++;
    const char *name = "?";
    /* A small table of the launch-relevant traps. Most others get '?'. */
    switch (trap & 0xFBFF) {     /* mask out auto-pop bit */
        case 0xA9F2: name = "_Launch"; break;
        case 0xA9F4: name = "_ExitToShell"; break;
        case 0xA9F0: name = "_LoadSeg"; break;
        case 0xA9F1: name = "_UnloadSeg"; break;
        case 0xA9C8: name = "_SysBeep"; break;
        case 0xA9C0: name = "_FInitQueue"; break;
        case 0xA970: name = "_GetNextEvent"; break;
        case 0xA971: name = "_WaitNextEvent"; break;
        case 0xA978: name = "_KeyTrans"; break;
        case 0xA93D: name = "_MenuKey"; break;
        case 0xA938: name = "_MenuSelect"; break;
        case 0xA94D: name = "_HiliteMenu"; break;
        case 0xA9D9: name = "_OpenDeskAcc"; break;
        case 0xA9D0: name = "_FlushVol"; break;
        case 0xA9A0: name = "_GetResource"; break;
        case 0xA9A1: name = "_Get1Resource"; break;
        case 0xA800: name = "_OpenRF"; break;
        case 0xA260: name = "_FSDispatch"; break;
        case 0xA1AD: name = "_Gestalt"; break;
        case 0xA9DC: name = "_SystemTask"; break;
        case 0xA8E0: name = "_OpenSelection"; break;
        case 0xA13D: name = "_LaunchControlPanel"; break;
        case 0xA88F: name = "_OSDispatch"; break;
    }
    fprintf(stderr, "[trap] cyc=%llu pc=%06X trap=%04X %s a0=%08X a7=%08X d0=%08X\n",
            (unsigned long long)cpu->cycles, cpu->pc, trap, name,
            cpu->a[0], cpu->a[7], cpu->d[0]);
}

static int scripted_run_file(mac_mem *m, m68k_cpu *cpu, const char *spath) {
    m->serial_sink = NULL;
    /* M6.X — trap-tracing: enable if MAC68K_TRACE_FROM / _TO are set. */
    if (getenv("MAC68K_TRACE_FROM") && getenv("MAC68K_TRACE_TO")) {
        g_trace_from = strtoull(getenv("MAC68K_TRACE_FROM"), NULL, 0);
        g_trace_to   = strtoull(getenv("MAC68K_TRACE_TO"),   NULL, 0);
        g_trace_count = 0;
        if (getenv("MAC68K_TRACE_MAX"))
            g_trace_max = (u32)strtoul(getenv("MAC68K_TRACE_MAX"), NULL, 0);
        m68k_trap_hook = trace_trap_hook;
        fprintf(stderr, "[trap] tracing %llu..%llu (max %u entries)\n",
                (unsigned long long)g_trace_from, (unsigned long long)g_trace_to,
                g_trace_max);
    }
    static script_ev script[256];
    int nev = 0;
    FILE *sf = fopen(spath, "r");
    if (!sf) { fprintf(stderr, "[script] cannot open %s\n", spath); return 1; }
    char line[256];
    while (nev < 256 && fgets(line, sizeof(line), sf)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        unsigned long long c; int x, y, b;
        if (*p == 'k') {
            /* Keyboard event: "k <cycle> <mac_keycode> <down>".
             * keycode accepts decimal, octal, or 0x-hex via %i. */
            int keycode, down;
            if (sscanf(p + 1, "%llu %i %i", &c, &keycode, &down) == 3) {
                script[nev].cyc = c; script[nev].kind = 1;
                script[nev].x = keycode; script[nev].y = 0;
                script[nev].btn = down;
                nev++;
            }
        } else if (sscanf(p, "%llu %d %d %d", &c, &x, &y, &b) == 4) {
            script[nev].cyc = c; script[nev].kind = 0;
            script[nev].x = x; script[nev].y = y; script[nev].btn = b;
            nev++;
        }
    }
    fclose(sf);

    const char *fdir  = getenv("MAC68K_FRAMEDIR") ? getenv("MAC68K_FRAMEDIR")
                                                  : "/tmp/spd";
    u64 frame_every = getenv("MAC68K_FRAME_EVERY")
                    ? strtoull(getenv("MAC68K_FRAME_EVERY"), NULL, 0)
                    : 20000000ull;
    const char *snap = getenv("MAC68K_SNAP");
    u64 snap_every = getenv("MAC68K_SNAP_EVERY")
                   ? strtoull(getenv("MAC68K_SNAP_EVERY"), NULL, 0) : 0;
    u64 snap_cyc = getenv("MAC68K_SNAP_CYCLE")
                 ? strtoull(getenv("MAC68K_SNAP_CYCLE"), NULL, 0) : 0;
    u64 end = getenv("MAC68K_END_CYCLE")
            ? strtoull(getenv("MAC68K_END_CYCLE"), NULL, 0)
            : (nev ? script[nev - 1].cyc + 400000000ull : 1500000000ull);

    int next = 0, frame = 0, snapn = 0;
    u64 last_dump = 0, last_snap = 0;
    fprintf(stderr, "[script] %d events from %s, end=%llu, framedir=%s\n",
            nev, spath, (unsigned long long)end, fdir);

    while (!cpu->halted && cpu->cycles < end) {
        m68k_run_until(cpu, cpu->cycles + 50000);   /* fine event timing */
        service_hd_insert(m, cpu);
        while (next < nev && cpu->cycles >= script[next].cyc) {
            if (script[next].kind == 1) {
                mac_key_event(m, script[next].x, script[next].btn != 0);
                fprintf(stderr, "[script] ev%d cyc=%llu key=0x%02X down=%d\n",
                        next, (unsigned long long)cpu->cycles,
                        script[next].x, script[next].btn);
            } else {
                mac_set_mouse(m, script[next].x, script[next].y,
                              script[next].btn != 0);
                fprintf(stderr, "[script] ev%d cyc=%llu mouse=(%d,%d) btn=%d\n",
                        next, (unsigned long long)cpu->cycles, script[next].x,
                        script[next].y, script[next].btn);
            }
            next++;
        }
        if (cpu->cycles - last_dump >= frame_every) {
            last_dump = cpu->cycles;
            char path[256];
            snprintf(path, sizeof(path), "%s/frame_%03d.bmp", fdir, frame++);
            write_screen_bmp(m, path);
        }
        if (snap) {
            if (snap_every && cpu->cycles - last_snap >= snap_every) {
                last_snap = cpu->cycles;
                char path[256];
                snprintf(path, sizeof(path), "%s.%03d", snap, snapn++);
                write_snapshot(m, cpu, path);
            } else if (!snap_every && snapn == 0 && cpu->cycles >= snap_cyc) {
                snapn = 1;
                write_snapshot(m, cpu, snap);
            }
        }
    }
    fprintf(stderr, "[script] done at cycle %llu (halted=%d)\n",
            (unsigned long long)cpu->cycles, cpu->halted);
    return 0;
}

/* Headless audio dump: write raw samples straight to a file. Reads
 * back with `play -t u8 -r 22255 -c 1 path`. */
static void fwrite_audio_cb(FILE *f, const u8 *samples, u32 n) {
    fwrite(samples, 1, n, f);
}

/* Headless serial Tx sink: prefix each byte with the channel letter
 * (A=modem, B=printer) so a single capture file shows both. */
static void scc_tx_cb(void *ctx, int channel, u8 byte) {
    FILE *f = (FILE *)ctx;
    fputc(channel == 1 ? 'A' : 'B', f);
    fputc((int)byte, f);
}

/* Sound sink: append 8-bit PCM samples to a shared ring buffer that
 * the server loop drains as MACGUI_PKT_AUDIO packets. ~6 VBLs of
 * buffer (≈100ms) is plenty for the GUI's SDL queue to smooth over
 * network/scheduling jitter. */
static u8     g_snd_buf[MAC_SND_SAMPLES_PER_VBL * 16];
static u32    g_snd_len;
static void server_snd_sink(void *ctx, const u8 *samples, u32 n) {
    (void)ctx;
    if (g_snd_len + n > sizeof(g_snd_buf)) {
        /* Drop oldest to avoid runaway latency if the GUI isn't draining. */
        u32 drop = g_snd_len + n - (u32)sizeof(g_snd_buf);
        memmove(g_snd_buf, g_snd_buf + drop, g_snd_len - drop);
        g_snd_len -= drop;
    }
    memcpy(g_snd_buf + g_snd_len, samples, n);
    g_snd_len += n;
}

static int server_loop(mac_mem *m, m68k_cpu *cpu) {
    if (getenv("MAC68K_MOUSESCRIPT"))
        return scripted_run_file(m, cpu, getenv("MAC68K_MOUSESCRIPT"));
    if (getenv("MAC68K_CPDEBUG")) return scripted_debug_run(m, cpu);
    /* stdin and stdout are the SAME socket (the GUI dup'd one socketpair
     * end onto both). So we must NOT set O_NONBLOCK on the descriptor —
     * that would also make our frame writes non-blocking and tear them.
     * Input is polled per-call with recv(MSG_DONTWAIT); output writes
     * stay blocking so whole packets always go out intact. */
    m->serial_sink = NULL;     /* stdout is the protocol stream now */
    m->snd_sink = server_snd_sink;
    m->snd_ctx  = NULL;

    u8 hello[4] = { (u8)MACGUI_SCREEN_W, (u8)(MACGUI_SCREEN_W >> 8),
                    (u8)MACGUI_SCREEN_H, (u8)(MACGUI_SCREEN_H >> 8) };
    send_packet(MACGUI_PKT_HELLO, hello, 4);

    static u8 inbuf[512];
    int inlen = 0;
    static u8 prevfb[MACGUI_FB_BYTES];
    static u8 fbcopy[MACGUI_FB_BYTES];
    static u8 rle[MACGUI_FB_BYTES * 2];
    memset(prevfb, 0x55, sizeof(prevfb));     /* force the first frame */

    double t0 = mono_seconds(), last_frame = 0, last_log = 0;
    /* Pacing baseline: target = pace_cyc0 + (now - pace_t0) * rate.
     * Rebased on every speed change so the CPU doesn't have to wait
     * for wallclock to catch up after a Max-speed burst. */
    double pace_t0 = t0;
    u64    pace_cyc0 = cpu->cycles;
    u32    last_speed_mult = g_speed_mult;
    fprintf(stderr, "[server] running — protocol on stdin/stdout\n");

    while (!cpu->halted) {
        double mono_now = mono_seconds();
        double now = mono_now - t0;
        if (g_speed_mult != last_speed_mult) {
            pace_t0 = mono_now;
            pace_cyc0 = cpu->cycles;
            last_speed_mult = g_speed_mult;
        }
        /* Pace the Mac to real time (7.8336 MHz) × g_speed_mult/100.
         * g_speed_mult==0 means "uncapped" — advance in fixed chunks
         * with no sleep so the interp/JIT runs as fast as the host
         * can go. The GUI's Speed menu changes g_speed_mult live. */
        if (g_speed_mult == 0) {
            m68k_run_until(cpu, cpu->cycles + 200000ull);
        } else {
            double dt = mono_now - pace_t0;
            u64 target = pace_cyc0 +
                (u64)(dt * 7833600.0 * (double)g_speed_mult / 100.0);
            if (cpu->cycles < target) m68k_run_until(cpu, target);
        }
        service_hd_insert(m, cpu);
        if (now - last_log >= 3.0) {
            last_log = now;
            fprintf(stderr, "[server] t=%.0fs cycles=%llu pc=%06X\n",
                    now, (unsigned long long)cpu->cycles, cpu->pc);
        }

        /* Drain any input packets (per-call non-blocking). */
        ssize_t n = recv(0, inbuf + inlen, sizeof(inbuf) - (size_t)inlen,
                         MSG_DONTWAIT);
        if (n == 0) break;                    /* GUI closed the pipe */
        if (n > 0) {
            inlen += (int)n;
            int off = 0;
            while (inlen - off >= 4) {
                u32 plen = inbuf[off+1] | (inbuf[off+2] << 8) | (inbuf[off+3] << 16);
                if ((u32)(inlen - off - 4) < plen) break;
                server_apply_packet(m, inbuf[off], inbuf + off + 4, plen);
                off += 4 + (int)plen;
            }
            if (off > 0) { memmove(inbuf, inbuf + off, (size_t)(inlen - off));
                           inlen -= off; }
        }

        /* Emit a framebuffer packet ~30x/second when it changed. */
        if (now - last_frame >= 0.033) {
            last_frame = now;
            for (u32 i = 0; i < MACGUI_FB_BYTES; i++)
                fbcopy[i] = m->ram[(m->fb_base + i) % m->ram_size];
            if (memcmp(fbcopy, prevfb, MACGUI_FB_BYTES) != 0) {
                size_t rl = macgui_rle_encode(fbcopy, MACGUI_FB_BYTES, rle);
                send_packet(MACGUI_PKT_FRAME, rle, (u32)rl);
                memcpy(prevfb, fbcopy, MACGUI_FB_BYTES);
                if (getenv("MAC_SERVER_DUMP"))
                    write_screen_bmp(m, getenv("MAC_SERVER_DUMP"));
            }
        }
        /* Drain any accumulated sound samples (~370 bytes per VBL). */
        if (g_snd_len > 0) {
            send_packet(MACGUI_PKT_AUDIO, g_snd_buf, g_snd_len);
            g_snd_len = 0;
        }
        usleep(2000);
    }
    return 0;
}

static void serial_cb(void *ctx, u8 b) {
    (void)ctx;
    fputc((int)b, stdout);
    fflush(stdout);
}

static int read_file(const char *path, u8 **out, u32 *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* Large enough for a hard-disk image (e.g. a ~1 GB Infinite HD), not
     * just a floppy; a bad ROM is caught separately by mac_load_rom. */
    if (sz <= 0 || sz > 1280L * 1024 * 1024) { fclose(f); return -2; }
    u8 *buf = (u8 *)malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -3; }
    fclose(f);
    *out = buf;
    *len = (u32)sz;
    return 0;
}

/* Load a machine snapshot (written by write_snapshot / the mvmac_diff
 * format) into `m` and `cpu`, replacing their contents. Used by
 * --load-snapshot to benchmark the engines on a frozen Speedometer
 * state. Returns 0 on success. */
static int load_snapshot(mac_mem *m, m68k_cpu *cpu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    u32 hdr[24], ram_size = 0, rom_size = 0;
    int rc = -2;
    if (fread(hdr, 4, 24, f) == 24 && hdr[0] == 0x4D414331u
        && fread(&ram_size, 4, 1, f) == 1 && ram_size && ram_size <= 64u*1024*1024) {
        mac_mem_free(m);
        mac_mem_init(m, ram_size);
        if (fread(m->ram, 1, ram_size, f) == ram_size
            && fread(&rom_size, 4, 1, f) == 1
            && rom_size && rom_size <= 4u*1024*1024) {
            free(m->rom);
            m->rom = (u8 *)malloc(rom_size);
            if (m->rom && fread(m->rom, 1, rom_size, f) == rom_size) {
                m->rom_size = rom_size;
                m->overlay  = false;
                memset(cpu, 0, sizeof(*cpu));
                for (int i = 0; i < 8; i++) {
                    cpu->d[i] = hdr[1 + i];
                    cpu->a[i] = hdr[9 + i];
                }
                cpu->pc = hdr[17]; cpu->sr  = hdr[18];
                cpu->usp = hdr[19]; cpu->ssp = hdr[20];
                cpu->mem = m; m->cpu = cpu;
                rc = 0;
            }
        }
    }
    fclose(f);
    return rc;
}

static void usage(const char *a0) {
    fprintf(stderr,
        "usage: %s [--interp|--jit] [--rom] [--max-cycles N] [--ram-mb M] [file]\n"
        "  --interp        run with the reference interpreter (default)\n"
        "  --jit           run with the JIT (via the host Xtensa simulator)\n"
        "  --rom           load `file` as a Macintosh ROM (mapped at 0x400000,\n"
        "                  overlaid at 0x0 for boot) instead of a raw RAM image\n"
        "  --disk D        disk image D (repeatable, up to 2). The first\n"
        "                  is the boot drive; the second is drive 2,\n"
        "                  inserted after boot. If only one is given,\n"
        "                  an 'InfiniteHD6.dsk' next to it is used as\n"
        "                  drive 2 automatically (disable: MAC68K_NO_HD).\n"
        "  --screenshot P  write the 512x342 framebuffer to BMP file P at exit\n"
        "  --max-cycles N  stop after N cycles (default 200M)\n"
        "  --load-snapshot S  load machine snapshot S and run it (for\n"
        "                  benchmarking an engine on a frozen state)\n"
        "  --ram-mb M      RAM size in MiB (default 1)\n"
        "  file            raw 68k image at 0x0, or a Mac ROM with --rom;\n"
        "                  omit for the built-in demo\n",
        a0);
}

int main(int argc, char **argv) {
    bool use_jit = false;
    bool is_rom = false;
    u64 max_cycles = 200000000ull;
    u32 ram_mb = 1;
    const char *rom_path = NULL;
    const char *disk_path[2] = { NULL, NULL };
    int  n_disks = 0;
    const char *shot_path = NULL;
    const char *audio_path = NULL;
    const char *serial_out_path = NULL;
    const char *serial_in_path  = NULL;
    const char *scsi_path = NULL;
    int         scsi_id   = 0;
    const char *snap_load = NULL;
    bool profile = false;
    bool diff_jit = false;
    bool diff_jit_trace = false;
    bool no_irq = false;
    bool server = false;
    /* M6.63 — JIT-arena tuning. Defaults match the existing
       M68K_JIT_ARENA_KB (1024 KB) bump-allocator behaviour. */
    u32 arena_kb = 1024;
    u8  evict_mode = CC_MODE_BUMP;
    u8  prefetch_mode = PREFETCH_NONE;   /* M6.71 / M6.72 */
    u8  prefetch_depth = 0;              /* 0 → dispatcher default (2 for CHAIN) */

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--interp")) use_jit = false;
        else if (!strcmp(argv[i], "--jit"))    use_jit = true;
        else if (!strcmp(argv[i], "--server")) server = true;
        else if (!strcmp(argv[i], "--rom"))    is_rom = true;
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) {
            if (n_disks < 2) disk_path[n_disks++] = argv[++i];
            else { fprintf(stderr, "at most 2 --disk images\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc)
            audio_path = argv[++i];
        else if (!strcmp(argv[i], "--serial-out") && i + 1 < argc)
            serial_out_path = argv[++i];
        else if (!strcmp(argv[i], "--serial-in") && i + 1 < argc)
            serial_in_path = argv[++i];
        else if (!strcmp(argv[i], "--scsi") && i + 1 < argc)
            scsi_path = argv[++i];
        else if (!strcmp(argv[i], "--scsi-id") && i + 1 < argc)
            scsi_id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-cycles") && i + 1 < argc)
            max_cycles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram-mb") && i + 1 < argc)
            ram_mb = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--load-snapshot") && i + 1 < argc)
            snap_load = argv[++i];
        else if (!strcmp(argv[i], "--profile")) profile = true;
        else if (!strcmp(argv[i], "--diff-jit")) diff_jit = true;
        else if (!strcmp(argv[i], "--diff-jit-trace")) diff_jit_trace = true;
        else if (!strcmp(argv[i], "--no-irq")) no_irq = true;
        else if (!strcmp(argv[i], "--arena-kb") && i + 1 < argc)
            arena_kb = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--evict") && i + 1 < argc) {
            const char *e = argv[++i];
            if      (!strcmp(e, "none") || !strcmp(e, "bump")) evict_mode = CC_MODE_BUMP;
            else if (!strcmp(e, "lru"))                         evict_mode = CC_MODE_LRU;
            else if (!strcmp(e, "fifo"))                        evict_mode = CC_MODE_FIFO;
            else { fprintf(stderr, "--evict: expect none|lru|fifo\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--prefetch") && i + 1 < argc) {
            const char *p = argv[++i];
            if      (!strcmp(p, "none"))   prefetch_mode = PREFETCH_NONE;
            else if (!strcmp(p, "static")) prefetch_mode = PREFETCH_STATIC;
            else if (!strcmp(p, "chain"))  prefetch_mode = PREFETCH_CHAIN;
            else { fprintf(stderr, "--prefetch: expect none|static|chain\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--prefetch-depth") && i + 1 < argc) {
            prefetch_depth = (u8)strtoul(argv[++i], NULL, 0);
        }
        else if (argv[i][0] == '-') { usage(argv[0]); return 1; }
        else rom_path = argv[i];
    }
    if (ram_mb == 0) ram_mb = 1;

    mac_mem mem;
    mac_mem_init(&mem, ram_mb * 1024u * 1024u);
    mem.serial_sink = serial_cb;

    if (rom_path) {
        u8 *rom = NULL; u32 rlen = 0;
        if (read_file(rom_path, &rom, &rlen) != 0) {
            fprintf(stderr, "failed to read %s\n", rom_path);
            return 2;
        }
        if (is_rom) {
            if (!mac_load_rom(&mem, rom, rlen)) {
                fprintf(stderr, "rom too large / invalid: %s\n", rom_path);
                free(rom);
                return 3;
            }
            fprintf(stderr, "[host] loaded Mac ROM %s (%u bytes) at 0x%06X, "
                            "overlaid at 0x0\n", rom_path, rlen, MAC_ROM_BASE);
        } else {
            mac_load_ram_image(&mem, 0, rom, rlen);
            fprintf(stderr, "[host] loaded %s (%u bytes) at 0x0\n",
                    rom_path, rlen);
        }
        free(rom);
        /* Drive 1 (the boot disk) goes in at reset. */
        if (n_disks >= 1) {
            u8 *disk = NULL; u32 dlen = 0;
            if (read_file(disk_path[0], &disk, &dlen) != 0) {
                fprintf(stderr, "failed to read disk %s\n", disk_path[0]);
                return 2;
            }
            if (mac_insert_disk_drive(&mem, 0, disk, dlen, false))
                fprintf(stderr, "[host] inserted floppy %s into drive 1 "
                        "(%u bytes)\n", disk_path[0], dlen);
            free(disk);
        }
        /* Resolve the drive-2 disk (inserted after boot — see
         * service_hd_insert): an explicit 2nd --disk / MAC68K_DISK2 /
         * MAC68K_HD, else an "InfiniteHD6.dsk" sitting next to the boot
         * disk. Disable the auto-detect with MAC68K_NO_HD. */
        if (getenv("MAC68K_DISK2"))    g_hd_path = getenv("MAC68K_DISK2");
        else if (n_disks >= 2)         g_hd_path = disk_path[1];
        else if (getenv("MAC68K_HD"))  g_hd_path = getenv("MAC68K_HD");
        else if (!getenv("MAC68K_NO_HD") && n_disks >= 1) {
            static char hd[1100];
            const char *slash = strrchr(disk_path[0], '/');
            int dl = slash ? (int)(slash - disk_path[0]) + 1 : 0;
            snprintf(hd, sizeof(hd), "%.*sInfiniteHD6.dsk",
                     dl, disk_path[0]);
            FILE *t = fopen(hd, "rb");
            if (t) { fclose(t); g_hd_path = hd; }
        }
        if (getenv("MAC68K_DISK2_CYCLE"))
            g_hd_cycle = strtoull(getenv("MAC68K_DISK2_CYCLE"), NULL, 0);
        if (g_hd_path)
            fprintf(stderr, "[host] drive 2 = %s (inserted after boot)\n",
                    g_hd_path);
    } else if (!snap_load) {
        u8 img[DEMO_ROM_MAX];
        u32 len = demo_rom_build(img, mem.fb_base);
        mac_load_ram_image(&mem, 0, img, len);
        fprintf(stderr, "[host] built-in demo (%u bytes), fb=0x%06X\n",
                len, mem.fb_base);
    }

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    if (snap_load) {
        if (load_snapshot(&mem, &cpu, snap_load) != 0) {
            fprintf(stderr, "failed to load snapshot %s\n", snap_load);
            return 5;
        }
        fprintf(stderr, "[host] loaded snapshot %s (pc=0x%06X, ram=%uK)\n",
                snap_load, cpu.pc, mem.ram_size >> 10);
    }
    fprintf(stderr, "[host] reset: PC=0x%06X SSP=0x%06X  mode=%s\n",
            cpu.pc, cpu.a[7], server ? "server" : use_jit ? "JIT" : "interp");

    /* --profile: interpret the workload and histogram executed opcodes,
     * so the JIT optimisation work can target the actually-hot ops. */
    if (profile) {
        static u32 freq[65536];
        u64 total = 0;
        while (cpu.cycles < max_cycles && !cpu.halted) {
            mac_mem_tick(cpu.mem, cpu.cycles);
            if (m68k_poll_interrupts(&cpu)) continue;
            if (sony_service(&cpu)) continue;
            if (cpu.stopped) { cpu.cycles += 64; continue; }
            freq[mac_read16(&mem, cpu.pc)]++;
            total++;
            m68k_step(&cpu);
        }
        /* Top 30 opcodes by execution count. */
        fprintf(stderr, "[profile] %llu instructions, top opcodes:\n",
                (unsigned long long)total);
        for (int rank = 0; rank < 30; rank++) {
            int best = -1; u32 bestn = 0;
            for (int o = 0; o < 65536; o++)
                if (freq[o] > bestn) { bestn = freq[o]; best = o; }
            if (best < 0) break;
            fprintf(stderr, "  %2d. op=%04X  %9u  %5.1f%%\n", rank + 1,
                    best, bestn, 100.0 * (double)bestn / (double)total);
            freq[best] = 0;
        }
        mac_mem_free(&mem);
        return 0;
    }

    /* --diff-jit-trace: run JIT one block at a time and compare to the
     * interpreter after each. Pinpoints the first diverging block — its
     * 68000 instructions are decoded and printed so the buggy op is
     * visible. Much slower than --diff-jit; use only for debugging. */
    if (diff_jit_trace && snap_load) {
        mac_mem mj; m68k_cpu cj;
        mac_mem_init(&mj, 4096);
        if (load_snapshot(&mj, &cj, snap_load) != 0) {
            fprintf(stderr, "trace: snapshot reload failed\n"); return 5;
        }
        if (no_irq) { cpu.sr |= 0x0700; cj.sr |= 0x0700; }
        m68k_dispatcher dd;
        if (!m68k_dispatcher_init(&dd, &cj)) {
            fprintf(stderr, "trace: jit init failed\n"); return 4;
        }
        m68k_dispatcher_set_prefetch(&dd, prefetch_mode, prefetch_depth);
        u64 step = 0;
        m68k_cpu pre_cj;
        while (cj.cycles < max_cycles && !cj.halted) {
            u32 pc_before = cj.pc;
            u64 cyc_before = cj.cycles;
            pre_cj = cj;
            /* sony.c uses a single global S.vm pointer for its
             * mac_mem accesses; in lockstep we must re-point it
             * to whichever engine's memory is about to run, or
             * a sony_service pseudo-exception / extension-trap
             * write would fire into the wrong mac_mem and report
             * a false divergence. (SONY_EXTN_BASE = 0xF40000 is
             * a real address Mac OS writes to via MOVE.L An,(An).) */
            sony_set_vm(&mj);
            m68k_dispatcher_run_until(&dd, cj.cycles + 1);   /* ≈ 1 block */
            sony_set_vm(&mem);
            m68k_run_until(&cpu, cj.cycles);
            int bad = 0;
            for (int r = 0; r < 8; r++) {
                if (cpu.d[r] != cj.d[r]) bad = 1;
                if (cpu.a[r] != cj.a[r]) bad = 1;
            }
            if (cpu.pc != cj.pc)                  bad = 1;
            if (cpu.sr != cj.sr)                  bad = 1;
            if (cpu.cycles != cj.cycles)          bad = 1;
            if (cpu.pending_irq != cj.pending_irq) bad = 1;
            if (cpu.usp != cj.usp || cpu.ssp != cj.ssp) bad = 1;
            if (memcmp(mem.ram, mj.ram, mem.ram_size) != 0) bad = 1;
            /* VIA state always diverges because mac_mem_tick is called
             * per-block in the JIT vs per-instruction in the interp. The
             * snapshot's code doesn't read VIA registers, so the divergence
             * doesn't propagate — exclude it from the comparison. */
            if (bad) {
                fprintf(stderr,
                    "[trace] DIVERGENCE at step %llu, block PC=0x%06X, "
                    "cycles %llu..%llu\n",
                    (unsigned long long)step, pc_before,
                    (unsigned long long)cyc_before, (unsigned long long)cj.cycles);
                for (int r = 0; r < 8; r++) {
                    if (cpu.d[r] != cj.d[r])
                        fprintf(stderr, "  D%d interp=%08X jit=%08X\n",
                                r, cpu.d[r], cj.d[r]);
                    if (cpu.a[r] != cj.a[r])
                        fprintf(stderr, "  A%d interp=%08X jit=%08X\n",
                                r, cpu.a[r], cj.a[r]);
                }
                if (cpu.pc != cj.pc)
                    fprintf(stderr, "  PC interp=%06X jit=%06X\n",
                            cpu.pc, cj.pc);
                if (cpu.sr != cj.sr)
                    fprintf(stderr, "  SR interp=%04X jit=%04X\n",
                            cpu.sr, cj.sr);
                if (cpu.usp != cj.usp)
                    fprintf(stderr, "  USP interp=%08X jit=%08X\n",
                            cpu.usp, cj.usp);
                if (cpu.ssp != cj.ssp)
                    fprintf(stderr, "  SSP interp=%08X jit=%08X\n",
                            cpu.ssp, cj.ssp);
                if (memcmp(mem.ram, mj.ram, mem.ram_size) != 0) {
                    int diffs = 0;
                    for (u32 a = 0; a < mem.ram_size && diffs < 6; a++) {
                        if (mem.ram[a] != mj.ram[a]) {
                            fprintf(stderr,
                                "  RAM[%06X] interp=%02X jit=%02X\n",
                                a, mem.ram[a], mj.ram[a]);
                            diffs++;
                        }
                    }
                }
                if (memcmp(&mem.via, &mj.via, sizeof(mem.via)) != 0) {
                    const u8 *vi = (const u8*)&mem.via;
                    const u8 *vj = (const u8*)&mj.via;
                    fprintf(stderr, "  VIA differs:\n");
                    int diffs = 0;
                    for (size_t b = 0; b < sizeof(mem.via) && diffs < 12; b++) {
                        if (vi[b] != vj[b]) {
                            fprintf(stderr,
                                "    +%02zX interp=%02X jit=%02X\n",
                                b, vi[b], vj[b]);
                            diffs++;
                        }
                    }
                }
                fprintf(stderr, "  pre-block state:\n");
                for (int r = 0; r < 8; r++)
                    fprintf(stderr, "    D%d=%08X  A%d=%08X\n",
                            r, pre_cj.d[r], r, pre_cj.a[r]);
                fprintf(stderr, "  block instructions:\n");
                u32 ppc = pc_before;
                for (int i = 0; i < 16; i++) {
                    m68k_decoded dec = m68k_decode_at(&cpu, ppc);
                    fprintf(stderr, "    PC=%06X  op=%04X%s\n",
                            ppc, dec.opcode, dec.ends_block ? " (ends)" : "");
                    if (dec.ends_block) break;
                    ppc += dec.length;
                }
                m68k_dispatcher_shutdown(&dd);
                return 2;
            }
            step++;
        }
        fprintf(stderr, "[trace] match through %llu cycles (%llu blocks)\n",
                (unsigned long long)cj.cycles, (unsigned long long)step);
        m68k_dispatcher_shutdown(&dd);
        return 0;
    }

    /* --diff-jit: run the loaded snapshot under the interpreter and the
     * JIT to the same cycle budget and report the first state mismatch.
     * Keep --max-cycles below one VBL period so no interrupt fires (an
     * interrupt taken at a block vs instruction boundary diverges the
     * engines legitimately). A real divergence is a JIT bug. */
    if (diff_jit && snap_load) {
        mac_mem mj; m68k_cpu cj;
        mac_mem_init(&mj, 4096);
        if (load_snapshot(&mj, &cj, snap_load) != 0) {
            fprintf(stderr, "diff-jit: snapshot reload failed\n"); return 5;
        }
        if (no_irq) { cpu.sr |= 0x0700; cj.sr |= 0x0700; }
        /* The JIT runs whole blocks, so it overshoots `max_cycles` to a
         * block boundary; run it first, then run the interpreter to the
         * JIT's exact final cycle count so both stop at the same point. */
        m68k_dispatcher dd;
        if (!m68k_dispatcher_init(&dd, &cj)) {
            fprintf(stderr, "diff-jit: jit init failed\n"); return 4;
        }
        m68k_dispatcher_set_prefetch(&dd, prefetch_mode, prefetch_depth);
        m68k_dispatcher_run_until(&dd, max_cycles);  /* JIT  */
        m68k_run_until(&cpu, cj.cycles);             /* interp to same cyc */
        int bad = 0;
        for (int r = 0; r < 8; r++) {
            if (cpu.d[r] != cj.d[r]) { fprintf(stderr,
                "  D%d interp=%08X jit=%08X\n", r, cpu.d[r], cj.d[r]); bad = 1; }
            if (cpu.a[r] != cj.a[r]) { fprintf(stderr,
                "  A%d interp=%08X jit=%08X\n", r, cpu.a[r], cj.a[r]); bad = 1; }
        }
        if (cpu.pc != cj.pc) { fprintf(stderr,
            "  PC interp=%06X jit=%06X\n", cpu.pc, cj.pc); bad = 1; }
        if ((cpu.sr & 0x1F) != (cj.sr & 0x1F)) { fprintf(stderr,
            "  CCR interp=%02X jit=%02X\n", cpu.sr & 0x1F, cj.sr & 0x1F); bad = 1; }
        if (cpu.cycles != cj.cycles) { fprintf(stderr,
            "  cycles interp=%llu jit=%llu\n",
            (unsigned long long)cpu.cycles, (unsigned long long)cj.cycles); bad = 1; }
        fprintf(stderr, "[diff-jit] %s at %llu cycles\n",
                bad ? "*** DIVERGENCE ***" : "match",
                (unsigned long long)max_cycles);
        m68k_dispatcher_shutdown(&dd);
        return bad ? 2 : 0;
    }

    if (server) {
        int rc = server_loop(&mem, &cpu);
        mac_mem_free(&mem);
        return rc;
    }

    /* --audio PATH: stream 8-bit unsigned PCM samples (22.255 kHz mono)
     * to a raw file as the Mac plays sound. Verify with `play -t u8
     * -r 22255 -c 1 PATH`. */
    FILE *audio_f = NULL;
    if (audio_path) {
        audio_f = fopen(audio_path, "wb");
        if (!audio_f) {
            fprintf(stderr, "[host] failed to open %s for audio: %s\n",
                    audio_path, strerror(errno));
            return 5;
        }
        mem.snd_sink = (void (*)(void *, const u8 *, u32))fwrite_audio_cb;
        mem.snd_ctx  = audio_f;
    }

    /* --scsi PATH [--scsi-id N]: attach a raw disk-image file as a SCSI
     * HD at target N (default 0). The image must be a multiple of 512
     * bytes; what's at it is up to you (an Apple-formatted HFS+driver
     * image will boot, a blank file works for write tests). */
    if (scsi_path) {
        u8 *img = NULL; u32 il = 0;
        if (read_file(scsi_path, &img, &il) != 0 || il == 0) {
            fprintf(stderr, "[host] failed to read SCSI image %s\n", scsi_path);
            return 5;
        }
        if (!mac_scsi_attach_disk(&mem.scsi, scsi_id, img, il, false)) {
            fprintf(stderr, "[host] SCSI attach failed (id=%d size=%u)\n",
                    scsi_id, il);
            free(img);
            return 5;
        }
        fprintf(stderr, "[host] SCSI target %d = %s (%u bytes / %u blocks)\n",
                scsi_id, scsi_path, il, il / 512u);
    }

    /* --serial-out PATH: capture SCC Tx (both channels) into one file
     * prefixed with the channel letter. --serial-in PATH: pre-load Rx
     * bytes for channel A (modem) — useful for scripted MIDI/printer
     * smoke tests. */
    FILE *ser_out_f = NULL;
    if (serial_out_path) {
        ser_out_f = fopen(serial_out_path, "wb");
        if (!ser_out_f) {
            fprintf(stderr, "[host] failed to open %s for serial out: %s\n",
                    serial_out_path, strerror(errno));
            return 5;
        }
        mem.scc.tx_sink = scc_tx_cb;
        mem.scc.tx_ctx  = ser_out_f;
    }
    if (serial_in_path) {
        FILE *sin = fopen(serial_in_path, "rb");
        if (!sin) {
            fprintf(stderr, "[host] failed to open %s for serial in: %s\n",
                    serial_in_path, strerror(errno));
            return 5;
        }
        int b;
        while ((b = fgetc(sin)) != EOF) mac_scc_rx(&mem, 1 /* A */, (u8)b);
        fclose(sin);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    m68k_dispatcher disp;
    if (use_jit && !m68k_dispatcher_init_ex(&disp, &cpu, arena_kb, evict_mode)) {
        fprintf(stderr, "JIT init failed (arena=%uKB evict=%u)\n",
                arena_kb, evict_mode);
        return 4;
    }
    if (use_jit) {
        m68k_dispatcher_set_prefetch(&disp, prefetch_mode, prefetch_depth);
        const char *en = evict_mode == CC_MODE_LRU  ? "lru"
                       : evict_mode == CC_MODE_FIFO ? "fifo"
                                                    : "none";
        const char *pn = prefetch_mode == PREFETCH_STATIC ? "static"
                       : prefetch_mode == PREFETCH_CHAIN  ? "chain"
                                                          : "none";
        fprintf(stderr, "[host] JIT arena=%uKB evict=%s prefetch=%s",
                arena_kb, en, pn);
        if (prefetch_mode == PREFETCH_CHAIN)
            fprintf(stderr, " depth=%u",
                    prefetch_depth ? prefetch_depth : 2);
        fprintf(stderr, "\n");
    }
    /* Run in chunks so the drive-2 disk can be inserted after boot. */
    while (cpu.cycles < max_cycles && !cpu.halted) {
        u64 next = cpu.cycles + 4000000ull;
        if (next > max_cycles) next = max_cycles;
        if (use_jit) m68k_dispatcher_run_until(&disp, next);
        else         m68k_run_until(&cpu, next);
        service_hd_insert(&mem, &cpu);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    double mhz = us > 0 ? (double)cpu.cycles / us : 0.0;

    fprintf(stderr,
        "\n[host] %s halted=%d exit_code=%d PC=0x%06X cycles=%llu "
        "elapsed=%.0fus throughput=%.2fMHz\n",
        use_jit ? "JIT" : "interp", cpu.halted, cpu.exit_code, cpu.pc,
        (unsigned long long)cpu.cycles, us, mhz);
    if (use_jit) {
        fprintf(stderr,
            "[host] blocks=%llu/%llu inline_ops=%llu helper_ops=%llu "
            "chain=%llu/%llu resets=%llu smc_inv=%llu\n",
            (unsigned long long)disp.blocks_compiled,
            (unsigned long long)disp.blocks_executed,
            (unsigned long long)disp.inline_ops_total,
            (unsigned long long)disp.helper_ops_total,
            (unsigned long long)disp.chain_hits,
            (unsigned long long)disp.chain_misses,
            (unsigned long long)disp.arena_resets,
            (unsigned long long)disp.smc_invalidations);
        fprintf(stderr, "[host] interp_fallbacks=%llu  chain_cache_matches=%llu/%llu  "
            "prefetch_compiles=%llu/%llu  prefetch_hits=%llu\n",
            (unsigned long long)disp.interp_fallbacks,
            (unsigned long long)disp.chain_cache_matches,
            (unsigned long long)disp.chain_hits,
            (unsigned long long)disp.prefetch_compiles,
            (unsigned long long)disp.blocks_compiled,
            (unsigned long long)disp.prefetch_hits);
        /* JIT-cost benchmark metric: estimated LX7 instructions to run the
         * workload, per emulated 68000 cycle. Lower is a faster JIT. */
        u64 cost = disp.xt_instrs
                 + disp.helper_calls * (u64)M68K_JIT_HELPER_LX7_COST;
        /* Corrected cost using cpu->instrs (the true count of m68k_step
         * helper invocations — includes the conditional-helper bridges
         * inside inline arms, which `disp.helper_calls` (a sum of
         * compile-time helper_ops counts) does not). */
        u64 real_cost = disp.xt_instrs
                      + (u64)cpu.instrs * (u64)M68K_JIT_HELPER_LX7_COST;
        fprintf(stderr,
            "[BENCH] jit_cost=%llu lx7 (xt=%llu helpers=%llu) "
            "cycles=%llu  lx7_per_cyc=%.3f  "
            "real_helpers=%llu real_lx7_per_cyc=%.3f\n",
            (unsigned long long)cost,
            (unsigned long long)disp.xt_instrs,
            (unsigned long long)disp.helper_calls,
            (unsigned long long)cpu.cycles,
            cpu.cycles ? (double)cost / (double)cpu.cycles : 0.0,
            (unsigned long long)cpu.instrs,
            cpu.cycles ? (double)real_cost / (double)cpu.cycles : 0.0);
        m68k_dispatcher_shutdown(&disp);
    } else {
        /* Interp baseline: each m68k_step is one "helper-call" equivalent,
         * weighted by M68K_JIT_HELPER_LX7_COST. Gives an apples-to-apples
         * lx7_per_cyc the JIT optimisation pass can beat. */
        u64 cost = cpu.instrs * (u64)M68K_JIT_HELPER_LX7_COST;
        fprintf(stderr,
            "[BENCH] interp_cost=%llu lx7 (instrs=%llu) "
            "cycles=%llu  lx7_per_cyc=%.3f\n",
            (unsigned long long)cost,
            (unsigned long long)cpu.instrs,
            (unsigned long long)cpu.cycles,
            cpu.cycles ? (double)cost / (double)cpu.cycles : 0.0);
    }

    if (shot_path) {
        write_screen_bmp(&mem, shot_path);
        fprintf(stderr, "[host] screenshot written to %s\n", shot_path);
    }
    if (audio_f) {
        long sz = ftell(audio_f);
        fclose(audio_f);
        fprintf(stderr, "[host] audio written to %s (%ld samples, %.2fs @22.255 kHz)\n",
                audio_path, sz, (double)sz / 22255.0);
    }
    if (ser_out_f) {
        long sz = ftell(ser_out_f);
        fclose(ser_out_f);
        fprintf(stderr, "[host] serial out: %ld bytes (each pair = chan+byte) -> %s\n",
                sz, serial_out_path);
    }

    /* exit_code mirrors the value the guest wrote to the debug exit port. */
    return cpu.exit_code;
}
