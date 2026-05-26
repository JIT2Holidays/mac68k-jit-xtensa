/* mac68k-jit-xtensa — SDL front-end.
 *
 * A desktop window that shows the emulated Macintosh screen and feeds it
 * mouse and keyboard input. The emulation itself runs in a separate
 * "backend" process that speaks the wire protocol in protocol.h:
 *
 *   --host   spawn `mac68k_host --server` and talk over a socket pair
 *            (fast — the emulator runs natively on this machine)
 *   --qemu   spawn qemu-system-xtensa running the ESP32-S3 firmware and
 *            talk over its emulated UART (the authentic Xtensa backend)
 *
 * Mouse moves are passed straight through to the Mac (it draws its own
 * cursor); the host keyboard is mapped to Mac key codes; and an on-screen
 * keyboard in the original Macintosh layout is drawn below the screen.
 *
 *   build:  see gui/CMakeLists.txt
 *   run:    ./mac_gui --host <rom> <disk>
 */

#include "protocol.h"
#include <stdbool.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libgen.h>

/* --- Macintosh M0110 key codes (mini vMac MKC values) ------------------ */

enum {
    MKC_A=0x00, MKC_B=0x0B, MKC_C=0x08, MKC_D=0x02, MKC_E=0x0E, MKC_F=0x03,
    MKC_G=0x05, MKC_H=0x04, MKC_I=0x22, MKC_J=0x26, MKC_K=0x28, MKC_L=0x25,
    MKC_M=0x2E, MKC_N=0x2D, MKC_O=0x1F, MKC_P=0x23, MKC_Q=0x0C, MKC_R=0x0F,
    MKC_S=0x01, MKC_T=0x11, MKC_U=0x20, MKC_V=0x09, MKC_W=0x0D, MKC_X=0x07,
    MKC_Y=0x10, MKC_Z=0x06,
    MKC_1=0x12, MKC_2=0x13, MKC_3=0x14, MKC_4=0x15, MKC_5=0x17, MKC_6=0x16,
    MKC_7=0x1A, MKC_8=0x1C, MKC_9=0x19, MKC_0=0x1D,
    MKC_Command=0x37, MKC_Shift=0x38, MKC_CapsLock=0x39, MKC_Option=0x3A,
    MKC_Space=0x31, MKC_Return=0x24, MKC_BackSpace=0x33, MKC_Tab=0x30,
    MKC_Left=0x7B, MKC_Right=0x7C, MKC_Down=0x7D, MKC_Up=0x7E,
    MKC_Minus=0x1B, MKC_Equal=0x18, MKC_BackSlash=0x2A, MKC_Comma=0x2B,
    MKC_Period=0x2F, MKC_Slash=0x2C, MKC_SemiColon=0x29, MKC_SingleQuote=0x27,
    MKC_LeftBracket=0x21, MKC_RightBracket=0x1E, MKC_Grave=0x32,
    MKC_Enter=0x4C, MKC_Escape=0x35
};

/* SDL scancode -> Mac key code. */
static int sdl_to_mkc(SDL_Scancode s) {
    switch (s) {
        case SDL_SCANCODE_A: return MKC_A; case SDL_SCANCODE_B: return MKC_B;
        case SDL_SCANCODE_C: return MKC_C; case SDL_SCANCODE_D: return MKC_D;
        case SDL_SCANCODE_E: return MKC_E; case SDL_SCANCODE_F: return MKC_F;
        case SDL_SCANCODE_G: return MKC_G; case SDL_SCANCODE_H: return MKC_H;
        case SDL_SCANCODE_I: return MKC_I; case SDL_SCANCODE_J: return MKC_J;
        case SDL_SCANCODE_K: return MKC_K; case SDL_SCANCODE_L: return MKC_L;
        case SDL_SCANCODE_M: return MKC_M; case SDL_SCANCODE_N: return MKC_N;
        case SDL_SCANCODE_O: return MKC_O; case SDL_SCANCODE_P: return MKC_P;
        case SDL_SCANCODE_Q: return MKC_Q; case SDL_SCANCODE_R: return MKC_R;
        case SDL_SCANCODE_S: return MKC_S; case SDL_SCANCODE_T: return MKC_T;
        case SDL_SCANCODE_U: return MKC_U; case SDL_SCANCODE_V: return MKC_V;
        case SDL_SCANCODE_W: return MKC_W; case SDL_SCANCODE_X: return MKC_X;
        case SDL_SCANCODE_Y: return MKC_Y; case SDL_SCANCODE_Z: return MKC_Z;
        case SDL_SCANCODE_1: return MKC_1; case SDL_SCANCODE_2: return MKC_2;
        case SDL_SCANCODE_3: return MKC_3; case SDL_SCANCODE_4: return MKC_4;
        case SDL_SCANCODE_5: return MKC_5; case SDL_SCANCODE_6: return MKC_6;
        case SDL_SCANCODE_7: return MKC_7; case SDL_SCANCODE_8: return MKC_8;
        case SDL_SCANCODE_9: return MKC_9; case SDL_SCANCODE_0: return MKC_0;
        case SDL_SCANCODE_RETURN: return MKC_Return;
        case SDL_SCANCODE_SPACE:  return MKC_Space;
        case SDL_SCANCODE_BACKSPACE: return MKC_BackSpace;
        case SDL_SCANCODE_TAB:    return MKC_Tab;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return MKC_Shift;
        case SDL_SCANCODE_CAPSLOCK: return MKC_CapsLock;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return MKC_Option;
        case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI:
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return MKC_Command;
        case SDL_SCANCODE_MINUS: return MKC_Minus;
        case SDL_SCANCODE_EQUALS: return MKC_Equal;
        case SDL_SCANCODE_LEFTBRACKET: return MKC_LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return MKC_RightBracket;
        case SDL_SCANCODE_BACKSLASH: return MKC_BackSlash;
        case SDL_SCANCODE_SEMICOLON: return MKC_SemiColon;
        case SDL_SCANCODE_APOSTROPHE: return MKC_SingleQuote;
        case SDL_SCANCODE_GRAVE: return MKC_Grave;
        case SDL_SCANCODE_COMMA: return MKC_Comma;
        case SDL_SCANCODE_PERIOD: return MKC_Period;
        case SDL_SCANCODE_SLASH: return MKC_Slash;
        case SDL_SCANCODE_LEFT: return MKC_Left;
        case SDL_SCANCODE_RIGHT: return MKC_Right;
        case SDL_SCANCODE_UP: return MKC_Up;
        case SDL_SCANCODE_DOWN: return MKC_Down;
        default: return -1;
    }
}

/* --- on-screen keyboard layout (Macintosh M0110, original layout) ------ */

typedef struct { float x, y, w; const char *label; int mkc; } KeyDef;

/* Each row is a list of {label, key-code, width-in-units}. A width of 0
 * is a half-unit gap. The classic Mac keyboard had no arrow cluster; a
 * small one is added at the lower right for practicality. */
typedef struct { const char *label; int mkc; float w; } KeyRow;

static const KeyRow row0[] = {
    {"`",MKC_Grave,1},{"1",MKC_1,1},{"2",MKC_2,1},{"3",MKC_3,1},{"4",MKC_4,1},
    {"5",MKC_5,1},{"6",MKC_6,1},{"7",MKC_7,1},{"8",MKC_8,1},{"9",MKC_9,1},
    {"0",MKC_0,1},{"-",MKC_Minus,1},{"=",MKC_Equal,1},{"delete",MKC_BackSpace,1.5f},
};
static const KeyRow row1[] = {
    {"tab",MKC_Tab,1.5f},{"Q",MKC_Q,1},{"W",MKC_W,1},{"E",MKC_E,1},{"R",MKC_R,1},
    {"T",MKC_T,1},{"Y",MKC_Y,1},{"U",MKC_U,1},{"I",MKC_I,1},{"O",MKC_O,1},
    {"P",MKC_P,1},{"[",MKC_LeftBracket,1},{"]",MKC_RightBracket,1},
    {"\\",MKC_BackSlash,1},
};
static const KeyRow row2[] = {
    {"caps",MKC_CapsLock,1.75f},{"A",MKC_A,1},{"S",MKC_S,1},{"D",MKC_D,1},
    {"F",MKC_F,1},{"G",MKC_G,1},{"H",MKC_H,1},{"J",MKC_J,1},{"K",MKC_K,1},
    {"L",MKC_L,1},{";",MKC_SemiColon,1},{"'",MKC_SingleQuote,1},
    {"return",MKC_Return,1.75f},
};
static const KeyRow row3[] = {
    {"shift",MKC_Shift,2.25f},{"Z",MKC_Z,1},{"X",MKC_X,1},{"C",MKC_C,1},
    {"V",MKC_V,1},{"B",MKC_B,1},{"N",MKC_N,1},{"M",MKC_M,1},{",",MKC_Comma,1},
    {".",MKC_Period,1},{"/",MKC_Slash,1},{"shift",MKC_Shift,2.25f},
};
static const KeyRow row4[] = {
    {"option",MKC_Option,1.5f},{"\xE2\x8C\x98",MKC_Command,1.5f},
    {"",MKC_Space,7},{"\xE2\x8C\x98",MKC_Command,1.5f},
    {"enter",MKC_Enter,1.5f},{"option",MKC_Option,1.5f},
};

#define MAX_KEYS 80
static KeyDef g_keys[MAX_KEYS];
static int    g_nkeys;
static SDL_Texture *g_keytex[MAX_KEYS];   /* cached label textures */
static int g_keylabel_w[MAX_KEYS], g_keylabel_h[MAX_KEYS];

static void build_keyboard(float unit, float kx, float ky, float kh) {
    const KeyRow *rows[5] = { row0, row1, row2, row3, row4 };
    int counts[5] = { (int)(sizeof(row0)/sizeof(row0[0])),
                      (int)(sizeof(row1)/sizeof(row1[0])),
                      (int)(sizeof(row2)/sizeof(row2[0])),
                      (int)(sizeof(row3)/sizeof(row3[0])),
                      (int)(sizeof(row4)/sizeof(row4[0])) };
    g_nkeys = 0;
    for (int r = 0; r < 5; r++) {
        float x = kx;
        float y = ky + r * (kh + 4);
        for (int i = 0; i < counts[r]; i++) {
            KeyDef *k = &g_keys[g_nkeys++];
            k->x = x; k->y = y; k->w = rows[r][i].w * unit - 4;
            k->label = rows[r][i].label; k->mkc = rows[r][i].mkc;
            x += rows[r][i].w * unit;
        }
    }
    /* arrow cluster, lower-right */
    float ax = kx + 22.0f * unit, ay = ky + 3 * (kh + 4);
    struct { const char *l; int m; float dx, dy; } arr[] = {
        {"\xE2\x86\x90",MKC_Left,0,1},{"\xE2\x86\x93",MKC_Down,1,1},
        {"\xE2\x86\x91",MKC_Up,1,0},{"\xE2\x86\x92",MKC_Right,2,1},
    };
    for (int i = 0; i < 4; i++) {
        KeyDef *k = &g_keys[g_nkeys++];
        k->x = ax + arr[i].dx * unit; k->y = ay + arr[i].dy * (kh + 4);
        k->w = unit - 4; k->label = arr[i].l; k->mkc = arr[i].m;
    }
}

/* --- backend connection ------------------------------------------------ */

static int   g_fd = -1;       /* bidirectional socket to the backend */
static pid_t g_child = -1;

static void spawn_host_backend(const char *self, const char *rom,
                               const char *disk) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair"); exit(1);
    }
    /* mac68k_host sits next to this executable. */
    char dir[1024]; snprintf(dir, sizeof(dir), "%s", self);
    char exe[1100]; snprintf(exe, sizeof(exe), "%s/mac68k_host", dirname(dir));

    pid_t pid = fork();
    if (pid == 0) {
        dup2(sv[1], 0); dup2(sv[1], 1);
        close(sv[0]); close(sv[1]);
        /* MAC_GUI_JIT=1 spawns the backend with --jit instead of the
         * default --interp, so the SDL front-end can drive the JIT
         * end-to-end (Finder, real workloads). Verified to render the
         * same frames as the interpreter post-MOVE.B-A7-fix. */
        if (getenv("MAC_GUI_JIT")) {
            execl(exe, exe, "--server", "--jit", "--rom",
                  "--disk", disk, rom, (char *)NULL);
        } else {
            execl(exe, exe, "--server", "--rom", "--disk", disk, rom,
                  (char *)NULL);
        }
        fprintf(stderr, "exec %s failed: %s\n", exe, strerror(errno));
        _exit(127);
    }
    close(sv[1]);
    g_fd = sv[0];
    g_child = pid;
}

static void spawn_qemu_backend(const char *flash) {
    /* qemu serves the firmware UART on a TCP port; we connect to it. */
    const int port = 5191;
    pid_t pid = fork();
    if (pid == 0) {
        char ser[64];
        snprintf(ser, sizeof(ser), "tcp:127.0.0.1:%d,server,nowait", port);
        execlp("qemu-system-xtensa", "qemu-system-xtensa",
               "-M", "esp32s3", "-m", "8M", "-nographic", "-no-reboot",
               "-drive", flash, "-serial", ser, (char *)NULL);
        fprintf(stderr, "exec qemu-system-xtensa failed\n");
        _exit(127);
    }
    g_child = pid;
    /* retry-connect until qemu's listener is up */
    for (int tries = 0; tries < 200; tries++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) { g_fd = s; return; }
        close(s);
        usleep(50000);
    }
    fprintf(stderr, "could not connect to qemu serial\n");
    exit(1);
}

/* Write the whole buffer, looping over partial / would-block writes —
 * input packets are tiny, so a brief spin is harmless. */
static void write_all(const uint8_t *buf, int len) {
    int off = 0;
    while (off < len) {
        ssize_t n = write(g_fd, buf + off, (size_t)(len - off));
        if (n > 0) { off += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR)) { usleep(200); continue; }
        return;                               /* backend gone */
    }
}

static void send_packet(int type, const uint8_t *p, int len) {
    if (g_fd < 0) return;
    uint8_t hdr[4] = { (uint8_t)type, (uint8_t)len,
                       (uint8_t)(len >> 8), (uint8_t)(len >> 16) };
    write_all(hdr, 4);
    if (len > 0) write_all(p, len);
}

/* --- incoming-packet reassembly --------------------------------------- */

static uint8_t g_inbuf[2 * MACGUI_FB_BYTES + 1024];
static int     g_inlen;

/* SDL audio device id (0 = no audio / opening failed). */
static SDL_AudioDeviceID g_audio = 0;

static void audio_handler(const uint8_t *samples, int n) {
    if (g_audio) SDL_QueueAudio(g_audio, samples, (Uint32)n);
}

/* Drain the socket; for each complete packet, invoke the handler. */
static void poll_backend(void (*on_frame)(const uint8_t *, int)) {
    for (;;) {
        ssize_t n = read(g_fd, g_inbuf + g_inlen,
                         sizeof(g_inbuf) - (size_t)g_inlen);
        if (n <= 0) break;
        g_inlen += (int)n;
        if (g_inlen == (int)sizeof(g_inbuf)) break;   /* full — process now */
    }
    int off = 0;
    while (g_inlen - off >= 4) {
        int len = g_inbuf[off+1] | (g_inbuf[off+2] << 8) | (g_inbuf[off+3] << 16);
        if (g_inlen - off - 4 < len) break;
        if (g_inbuf[off] == MACGUI_PKT_FRAME)
            on_frame(g_inbuf + off + 4, len);
        else if (g_inbuf[off] == MACGUI_PKT_AUDIO)
            audio_handler(g_inbuf + off + 4, len);
        off += 4 + len;
    }
    if (off > 0) {
        memmove(g_inbuf, g_inbuf + off, (size_t)(g_inlen - off));
        g_inlen -= off;
    }
}

/* --- the framebuffer ARGB texture ------------------------------------- */

static uint32_t g_pixels[MACGUI_SCREEN_W * MACGUI_SCREEN_H];

static void dump_fb_bmp(const uint8_t *fb, const char *path) {
    int W = MACGUI_SCREEN_W, H = MACGUI_SCREEN_H, row = (W + 3) & ~3;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char h[54] = {0};
    h[0]='B'; h[1]='M'; *(int*)(h+2)=54+1024+row*H; *(int*)(h+10)=54+1024;
    *(int*)(h+14)=40; *(int*)(h+18)=W; *(int*)(h+22)=H; h[26]=1; h[28]=8;
    fwrite(h,1,54,f);
    for (int i=0;i<256;i++){ unsigned char p[4]={(unsigned char)i,(unsigned char)i,
        (unsigned char)i,0}; fwrite(p,1,4,f); }
    unsigned char *ln = calloc(1,(size_t)row);
    for (int y=H-1;y>=0;y--){ for (int x=0;x<W;x++){
        int bit=(fb[y*(W/8)+(x>>3)]>>(7-(x&7)))&1; ln[x]=bit?0:255; }
        fwrite(ln,1,(size_t)row,f); }
    free(ln); fclose(f);
}

static void frame_handler(const uint8_t *rle, int len) {
    static uint8_t fb[MACGUI_FB_BYTES];
    macgui_rle_decode(rle, (size_t)len, fb, sizeof(fb));
    if (getenv("MAC_GUI_DUMP")) dump_fb_bmp(fb, getenv("MAC_GUI_DUMP"));
    for (int y = 0; y < MACGUI_SCREEN_H; y++) {
        for (int x = 0; x < MACGUI_SCREEN_W; x++) {
            uint8_t byte = fb[y * (MACGUI_SCREEN_W / 8) + (x >> 3)];
            int bit = (byte >> (7 - (x & 7))) & 1;   /* 1 = black */
            g_pixels[y * MACGUI_SCREEN_W + x] = bit ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

/* --- main -------------------------------------------------------------- */

int main(int argc, char **argv) {
    bool use_qemu = false;
    const char *rom = NULL, *disk = NULL, *flash = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--qemu")) use_qemu = true;
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) flash = argv[++i];
        else if (!rom) rom = argv[i];
        else if (!disk) disk = argv[i];
    }
    if (use_qemu) {
        if (!flash) { fprintf(stderr, "usage: mac_gui --qemu --flash <img>\n");
                      return 1; }
    } else if (!rom || !disk) {
        fprintf(stderr, "usage: mac_gui <rom> <disk>\n"
                        "       mac_gui --qemu --flash <flash.bin>\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    if (use_qemu) spawn_qemu_backend(flash);
    else          spawn_host_backend(argv[0], rom, disk);
    fcntl(g_fd, F_SETFL, O_NONBLOCK);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    /* Mac Plus sound: 22255 Hz, 8-bit unsigned mono. Open with a
     * matching spec so SDL doesn't resample. The backend emits exactly
     * one VBL's worth of samples (370 bytes) per packet at 60Hz, so
     * the effective rate is 22200 Hz — within SDL's resampler tolerance
     * if the spec is unavailable. MAC_GUI_NO_AUDIO disables. */
    if (!getenv("MAC_GUI_NO_AUDIO")) {
        SDL_AudioSpec want = {0}, have = {0};
        want.freq = 22255;
        want.format = AUDIO_U8;
        want.channels = 1;
        want.samples = 1024;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audio == 0)
            fprintf(stderr, "[gui] SDL_OpenAudioDevice failed: %s — silent\n",
                    SDL_GetError());
        else
            SDL_PauseAudioDevice(g_audio, 0);
    }
    TTF_Init();
    TTF_Font *font = TTF_OpenFont("/System/Library/Fonts/Monaco.ttf", 13);

    const int scale = 2;
    const int sw = MACGUI_SCREEN_W * scale, sh = MACGUI_SCREEN_H * scale;
    const float unit = sw / 28.0f;          /* keyboard key unit */
    const int kh = (int)(unit * 0.9f);
    const int kbd_y = sh + 12;
    const int win_w = sw;
    const int win_h = kbd_y + 5 * (kh + 4) + 12;
    build_keyboard(unit, 8, (float)kbd_y, (float)kh);

    SDL_Window *win = SDL_CreateWindow("Macintosh — mac68k-jit-xtensa",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *screen = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, MACGUI_SCREEN_W, MACGUI_SCREEN_H);

    /* render the key labels once */
    if (font) {
        SDL_Color fg = { 30, 30, 30, 255 };
        for (int i = 0; i < g_nkeys; i++) {
            if (!g_keys[i].label || !g_keys[i].label[0]) continue;
            SDL_Surface *s = TTF_RenderUTF8_Blended(font, g_keys[i].label, fg);
            if (s) {
                g_keytex[i] = SDL_CreateTextureFromSurface(ren, s);
                g_keylabel_w[i] = s->w; g_keylabel_h[i] = s->h;
                SDL_FreeSurface(s);
            }
        }
    }

    for (int i = 0; i < MACGUI_SCREEN_W * MACGUI_SCREEN_H; i++)
        g_pixels[i] = 0xFF888888u;

    int mouse_btn = 0, osk_key = -1;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.y < sh) {           /* over the Mac screen */
                    uint8_t p[5];
                    int mx = e.motion.x / scale, my = e.motion.y / scale;
                    p[0]=(uint8_t)mx; p[1]=(uint8_t)(mx>>8);
                    p[2]=(uint8_t)my; p[3]=(uint8_t)(my>>8); p[4]=mouse_btn;
                    send_packet(MACGUI_PKT_MOUSE, p, 5);
                }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                bool down = (e.type == SDL_MOUSEBUTTONDOWN);
                if (e.button.y < sh) {           /* Mac screen — mouse click */
                    mouse_btn = down ? 1 : 0;
                    uint8_t p[5];
                    int mx = e.button.x / scale, my = e.button.y / scale;
                    p[0]=(uint8_t)mx; p[1]=(uint8_t)(mx>>8);
                    p[2]=(uint8_t)my; p[3]=(uint8_t)(my>>8); p[4]=mouse_btn;
                    send_packet(MACGUI_PKT_MOUSE, p, 5);
                } else {                         /* on-screen keyboard */
                    if (down) {
                        for (int i = 0; i < g_nkeys; i++) {
                            KeyDef *k = &g_keys[i];
                            if (e.button.x >= k->x && e.button.x < k->x + k->w &&
                                e.button.y >= k->y && e.button.y < k->y + kh) {
                                uint8_t p[2] = { (uint8_t)k->mkc, 1 };
                                send_packet(MACGUI_PKT_KEY, p, 2);
                                osk_key = i;
                                break;
                            }
                        }
                    } else if (osk_key >= 0) {
                        uint8_t p[2] = { (uint8_t)g_keys[osk_key].mkc, 0 };
                        send_packet(MACGUI_PKT_KEY, p, 2);
                        osk_key = -1;
                    }
                }
            }
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                if (!e.key.repeat) {
                    int mkc = sdl_to_mkc(e.key.keysym.scancode);
                    if (mkc >= 0) {
                        uint8_t p[2] = { (uint8_t)mkc,
                                         (uint8_t)(e.type == SDL_KEYDOWN) };
                        send_packet(MACGUI_PKT_KEY, p, 2);
                    }
                }
            }
        }

        poll_backend(frame_handler);

        /* hide the host cursor over the Mac screen — the Mac draws its own */
        int hx, hy; SDL_GetMouseState(&hx, &hy);
        SDL_ShowCursor(hy < sh ? SDL_DISABLE : SDL_ENABLE);

        SDL_UpdateTexture(screen, NULL, g_pixels, MACGUI_SCREEN_W * 4);
        SDL_SetRenderDrawColor(ren, 0xC0, 0xC0, 0xC0, 0xFF);
        SDL_RenderClear(ren);
        SDL_Rect dst = { 0, 0, sw, sh };
        SDL_RenderCopy(ren, screen, NULL, &dst);

        /* the on-screen keyboard */
        for (int i = 0; i < g_nkeys; i++) {
            KeyDef *k = &g_keys[i];
            SDL_Rect r = { (int)k->x, (int)k->y, (int)k->w, kh };
            bool hot = (i == osk_key);
            SDL_SetRenderDrawColor(ren, hot ? 0x60 : 0xF4, hot ? 0x60 : 0xF4,
                                   hot ? 0x60 : 0xF4, 0xFF);
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, 0x40, 0x40, 0x40, 0xFF);
            SDL_RenderDrawRect(ren, &r);
            if (g_keytex[i]) {
                SDL_Rect lr = { r.x + (r.w - g_keylabel_w[i]) / 2,
                                r.y + (kh - g_keylabel_h[i]) / 2,
                                g_keylabel_w[i], g_keylabel_h[i] };
                SDL_RenderCopy(ren, g_keytex[i], NULL, &lr);
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);

        int st;
        if (g_child > 0 && waitpid(g_child, &st, WNOHANG) == g_child)
            running = false;
    }

    if (g_child > 0) { kill(g_child, SIGTERM); waitpid(g_child, NULL, 0); }
    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
