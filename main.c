// Speedrun Timer for Nintendo 3DS
// Welcome screen with your own games, touch buttons, settings, background music.
//
// Physical buttons (touch buttons do the same):
//   Welcome:  D-pad = move   A = open game   X = settings   START = exit
//   Run:      A = start/split   SELECT = reset   X = settings   START = save & exit
//   Popups:   A = yes   B = no          Settings: B = back
//
// SD card layout (sdmc:/3ds/speedtimer/):
//   settings.txt              your settings
//   music.wav                 optional: your own music (16-bit PCM WAV, mono or stereo)
//   games/<game>/game.txt     name + category
//   games/<game>/splits.txt   one segment name per line
//   games/<game>/data.txt     saved PB and gold splits

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_SPLITS  20
#define NAME_LEN    30
#define MAX_GAMES   30
#define DIR_PATH    "sdmc:/3ds/speedtimer"
#define GAMES_DIR   DIR_PATH "/games"
#define SETTINGS_FILE DIR_PATH "/settings.txt"
#define MUSIC_FILE  DIR_PATH "/music.wav"

// ---------- palette ----------
#define Z 0.5f
#define C_BG_TOP     C2D_Color32(10, 24, 17, 255)
#define C_BG_BOT     C2D_Color32(17, 38, 27, 255)
#define C_PANEL      C2D_Color32(20, 46, 32, 255)
#define C_PANEL_LT   C2D_Color32(30, 68, 46, 255)
#define C_ROW        C2D_Color32(15, 34, 24, 255)
#define C_ROW_ALT    C2D_Color32(19, 41, 29, 255)
#define C_GREEN      C2D_Color32(76, 196, 84, 255)
#define C_GREEN_DK   C2D_Color32(14, 40, 22, 255)
#define C_PURPLE     C2D_Color32(142, 124, 195, 255)
#define C_PURPLE_DK  C2D_Color32(52, 40, 88, 255)
#define C_TEXT       C2D_Color32(238, 243, 230, 255)
#define C_DIM        C2D_Color32(132, 160, 142, 255)
#define C_AHEAD      C2D_Color32(98, 230, 168, 255)
#define C_BEHIND     C2D_Color32(255, 107, 107, 255)
#define C_GOLD       C2D_Color32(255, 210, 77, 255)
#define C_SHADE      C2D_Color32(0, 0, 0, 170)

typedef enum { STATE_IDLE, STATE_RUNNING, STATE_FINISHED } RunState;
typedef enum { SCR_WELCOME, SCR_RUN, SCR_SETTINGS } Screen;
typedef enum { M_NONE, M_RESET_RUN, M_RESET_GAME, M_RESET_ALL, M_DELETE_GAME } Modal;

typedef struct { char id[48]; char name[32]; char cat[24]; } GameInfo;

// ---------- global state ----------
static Screen screen = SCR_WELCOME;
static Screen settingsFrom = SCR_WELCOME;
static Modal  modal = M_NONE;
static bool   quit = false;

static GameInfo games[MAX_GAMES];
static int  gameCount = 0, selGame = 0, listFirst = 0;
static bool gameLoaded = false;
static char gameDir[128], gameName[32], gameCat[24];

static char names[MAX_SPLITS][NAME_LEN];
static int  count = 0;
static s64  pb[MAX_SPLITS];      // personal best, cumulative ms (-1 = none)
static s64  gold[MAX_SPLITS];    // best single-segment ms (-1 = none)
static s64  cur[MAX_SPLITS];     // this run, cumulative ms
static bool goldHit[MAX_SPLITS]; // did this run set a new gold here?
static RunState state = STATE_IDLE;
static int  idx = 0;
static s64  startTime = 0, finalTime = 0;
static bool isPB = false;

// settings
static bool confirmReset = true;
static bool musicOn = true;
static int  volIdx = 1;
static const int volLevels[4] = {25, 50, 75, 100};

static char toast[80];
static int  toastFrames = 0;
static void show_toast(const char *msg) {
    snprintf(toast, sizeof toast, "%s", msg);
    toastFrames = 150;
}

// ---------- formatting ----------

static void fmt_time(char *out, size_t n, s64 ms) {
    if (ms < 0) { snprintf(out, n, "-:--.--"); return; }
    int cs = (int)((ms / 10) % 100);
    int s  = (int)((ms / 1000) % 60);
    int m  = (int)((ms / 60000) % 60);
    int h  = (int)(ms / 3600000);
    if (h > 0) snprintf(out, n, "%d:%02d:%02d.%02d", h, m, s, cs);
    else       snprintf(out, n, "%d:%02d.%02d", m, s, cs);
}

static void fmt_delta(char *out, size_t n, s64 d) {
    char t[24];
    fmt_time(t, sizeof t, d < 0 ? -d : d);
    snprintf(out, n, "%c%s", d < 0 ? '-' : '+', t);
}

// ---------- settings file ----------

static void load_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    int a, b, c;
    if (fscanf(f, "%d %d %d", &a, &b, &c) == 3) {
        confirmReset = a != 0;
        musicOn = b != 0;
        volIdx = (c >= 0 && c < 4) ? c : 1;
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", confirmReset ? 1 : 0, musicOn ? 1 : 0, volIdx);
    fclose(f);
}

// ---------- audio (background music) ----------

#define MUSIC_RATE 16000
static bool audioOK = false;
static const char *audioStatus = "Music not started";
static ndspWaveBuf wave;
static s16 *audioBuf = NULL;

static float midi_hz(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

// kind 0 = soft sine (bass, bells), kind 1 = triangle (arpeggio)
static void add_note(float *buf, int total, int start, int len,
                     float hz, float amp, float decay, int kind) {
    for (int i = 0; i < len && start + i < total; i++) {
        float t = (float)i / MUSIC_RATE;
        float ph = hz * t;
        float w = (kind == 0) ? sinf(6.2831853f * ph)
                              : (4.0f * fabsf(ph - floorf(ph) - 0.5f) - 1.0f);
        float env = expf(-decay * t);
        float atk = i < 80 ? i / 80.0f : 1.0f;
        int rem = len - i;
        float rel = rem < 160 ? rem / 160.0f : 1.0f;
        buf[start + i] += w * amp * env * atk * rel;
    }
}

// A short, original, spooky minor-key loop (Am - F - Dm - E), made at startup.
static s16 *generate_music(int *nsamples) {
    const int bpm = 108;
    const int beat = MUSIC_RATE * 60 / bpm;
    const int bars = 8;
    int total = bars * 4 * beat;
    float *f = (float *)calloc(total, sizeof(float));
    if (!f) return NULL;
    static const int chords[4][3] = {{57,60,64},{53,57,60},{50,53,57},{52,56,59}};
    static const int bassRoot[4]  = {45, 41, 38, 40};
    static const int mel[4][4]    = {{76,72,69,72},{77,72,69,72},{74,69,65,69},{76,71,68,71}};
    static const int pat[8]       = {0, 1, 2, 1, 0, 1, 2, 1};
    for (int bar = 0; bar < bars; bar++) {
        int ch = bar % 4;
        int b0 = bar * 4 * beat;
        add_note(f, total, b0, 4 * beat, midi_hz(bassRoot[ch]), 0.35f, 0.5f, 0);
        for (int j = 0; j < 8; j++) {
            int n = chords[ch][pat[j]] + (j >= 4 ? 12 : 0);
            add_note(f, total, b0 + j * beat / 2, beat / 2, midi_hz(n), 0.16f, 6.0f, 1);
        }
        if (bar >= 4) {
            for (int j = 0; j < 4; j++)
                add_note(f, total, b0 + j * beat, beat, midi_hz(mel[ch][j]), 0.20f, 2.5f, 0);
        }
    }
    s16 *out = (s16 *)linearAlloc(total * sizeof(s16));
    if (out) {
        for (int i = 0; i < total; i++) {
            float v = f[i];
            if (v > 0.95f) v = 0.95f;
            if (v < -0.95f) v = -0.95f;
            out[i] = (s16)(v * 32000.0f);
        }
        *nsamples = total;
    }
    free(f);
    return out;
}

// Loads a 16-bit PCM WAV (mono or stereo). Returns NULL if it can't.
static s16 *load_wav(const char *path, int *nsamples, int *rate, int *channels) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    u8 hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return NULL;
    }
    unsigned fmtTag = 0, ch = 0, bits = 0, sr = 0;
    long dataPos = -1; unsigned dataSize = 0;
    for (;;) {
        u8 ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        unsigned sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((unsigned)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4) && sz >= 16) {
            u8 fm[16];
            if (fread(fm, 1, 16, f) != 16) break;
            fmtTag = fm[0] | (fm[1] << 8);
            ch     = fm[2] | (fm[3] << 8);
            sr     = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((unsigned)fm[7] << 24);
            bits   = fm[14] | (fm[15] << 8);
            if (sz > 16) fseek(f, sz - 16 + (sz & 1), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            dataPos = ftell(f); dataSize = sz; break;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    if (dataPos < 0 || fmtTag != 1 || bits != 16 || (ch != 1 && ch != 2) || sr == 0) {
        fclose(f); return NULL;
    }
    if (dataSize > 16u * 1024 * 1024) dataSize = 16u * 1024 * 1024;  // cap memory use
    dataSize -= dataSize % (ch * 2);
    if (dataSize == 0) { fclose(f); return NULL; }
    s16 *buf = (s16 *)linearAlloc(dataSize);
    if (!buf) { fclose(f); return NULL; }
    fseek(f, dataPos, SEEK_SET);
    size_t got = fread(buf, 1, dataSize, f);
    fclose(f);
    if (got != dataSize) { linearFree(buf); return NULL; }
    *nsamples = (int)(dataSize / (ch * 2));
    *rate = (int)sr;
    *channels = (int)ch;
    return buf;
}

static void audio_apply(void) {
    if (!audioOK) return;
    float mix[12] = {0};
    float v = volLevels[volIdx] / 100.0f;
    mix[0] = v; mix[1] = v;
    ndspChnSetMix(0, mix);
    ndspChnSetPaused(0, !musicOn);
}

static void audio_init(void) {
    if (R_FAILED(ndspInit())) {
        audioStatus = "No DSP firmware found (see Rosalina > Misc > Dump DSP)";
        return;
    }
    int n = 0, rate = MUSIC_RATE, ch = 1;
    audioBuf = load_wav(MUSIC_FILE, &n, &rate, &ch);
    if (audioBuf) {
        audioStatus = "Playing music.wav";
    } else {
        ch = 1; rate = MUSIC_RATE;
        audioBuf = generate_music(&n);
        audioStatus = "Playing built-in tune";
    }
    if (!audioBuf) {
        audioStatus = "Not enough memory for music";
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)rate);
    ndspChnSetFormat(0, ch == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    memset(&wave, 0, sizeof wave);
    wave.data_vaddr = audioBuf;
    wave.nsamples = n;
    wave.looping = true;
    DSP_FlushDataCache(audioBuf, n * ch * 2);
    ndspChnWaveBufAdd(0, &wave);
    audioOK = true;
    audio_apply();
}

static void audio_exit(void) {
    if (audioOK) {
        ndspChnWaveBufClear(0);
        ndspExit();
    }
    if (audioBuf) linearFree(audioBuf);
}

// ---------- games on the SD card ----------

static void game_path(const char *id, const char *file, char *out, size_t n) {
    snprintf(out, n, GAMES_DIR "/%s/%s", id, file);
}

static int cmp_games(const void *a, const void *b) {
    return strcasecmp(((const GameInfo *)a)->name, ((const GameInfo *)b)->name);
}

static void trim_line(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ')) s[--len] = 0;
}

static void scan_games(void) {
    gameCount = 0;
    DIR *d = opendir(GAMES_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && gameCount < MAX_GAMES) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= sizeof games[0].id) continue;
        char p[160];
        game_path(e->d_name, "game.txt", p, sizeof p);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        GameInfo g;
        memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "%s", e->d_name);
        char line[96];
        if (fgets(line, sizeof line, f)) { trim_line(line); snprintf(g.name, sizeof g.name, "%s", line); }
        if (fgets(line, sizeof line, f)) { trim_line(line); snprintf(g.cat,  sizeof g.cat,  "%s", line); }
        fclose(f);
        if (g.name[0] == 0) continue;
        games[gameCount++] = g;
    }
    closedir(d);
    qsort(games, gameCount, sizeof games[0], cmp_games);
    if (selGame >= gameCount) selGame = gameCount - 1;
    if (selGame < 0) selGame = 0;
}

static void make_id(const char *name, char *id, size_t n) {
    size_t k = 0;
    for (const char *p = name; *p && k < n - 6; p++) {
        if (isalnum((unsigned char)*p)) id[k++] = (char)tolower((unsigned char)*p);
        else if (k > 0 && id[k-1] != '_') id[k++] = '_';
    }
    while (k > 0 && id[k-1] == '_') k--;
    if (k == 0) { strcpy(id, "game"); k = 4; }
    id[k] = 0;
    // make it unique
    char base[48]; snprintf(base, sizeof base, "%s", id);
    for (int i = 2; i < 100; i++) {
        char p[160]; struct stat st;
        snprintf(p, sizeof p, GAMES_DIR "/%s", id);
        if (stat(p, &st) != 0) return;
        snprintf(id, n, "%s_%d", base, i);
    }
}

// If you used an earlier version, bring its splits and PB into the new layout.
static void migrate_old(void) {
    struct stat st;
    if (stat(DIR_PATH "/splits.txt", &st) != 0) return;
    scan_games();
    if (gameCount > 0) return;
    char dir[160];
    snprintf(dir, sizeof dir, GAMES_DIR "/luigis_mansion_dark_moon");
    mkdir(dir, 0777);
    FILE *f = fopen(GAMES_DIR "/luigis_mansion_dark_moon/game.txt", "w");
    if (f) { fprintf(f, "Luigi's Mansion: Dark Moon\nAny%%\n"); fclose(f); }
    rename(DIR_PATH "/splits.txt", GAMES_DIR "/luigis_mansion_dark_moon/splits.txt");
    rename(DIR_PATH "/data.txt",   GAMES_DIR "/luigis_mansion_dark_moon/data.txt");
}

static void delete_game_files(const char *id) {
    char p[160];
    game_path(id, "game.txt", p, sizeof p);   remove(p);
    game_path(id, "splits.txt", p, sizeof p); remove(p);
    game_path(id, "data.txt", p, sizeof p);   remove(p);
    snprintf(p, sizeof p, GAMES_DIR "/%s", id);
    rmdir(p);
}

// ---------- current game: splits, PB, golds ----------

static void clear_data(void) {
    for (int i = 0; i < MAX_SPLITS; i++) { pb[i] = -1; gold[i] = -1; }
}

static void load_splits(void) {
    count = 0;
    char p[200];
    snprintf(p, sizeof p, "%s/splits.txt", gameDir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (count < MAX_SPLITS && fgets(line, sizeof line, f)) {
        trim_line(line);
        if (line[0] == 0) continue;
        snprintf(names[count], NAME_LEN, "%s", line);
        count++;
    }
    fclose(f);
}

static void load_data(void) {
    clear_data();
    char p[200];
    snprintf(p, sizeof p, "%s/data.txt", gameDir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    s64 tp[MAX_SPLITS], tg[MAX_SPLITS];
    long long a, b;
    int n = 0;
    while (n < count && fscanf(f, "%lld %lld", &a, &b) == 2) { tp[n] = a; tg[n] = b; n++; }
    bool extra = (fscanf(f, "%lld %lld", &a, &b) == 2);
    fclose(f);
    if (n == count && !extra)
        for (int i = 0; i < count; i++) { pb[i] = tp[i]; gold[i] = tg[i]; }
}

static void save_data(void) {
    if (!gameLoaded) return;
    char p[200];
    snprintf(p, sizeof p, "%s/data.txt", gameDir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%lld %lld\n", (long long)pb[i], (long long)gold[i]);
    fclose(f);
}

static void commit_run(void) {
    if (!gameLoaded) return;
    if (state == STATE_FINISHED && isPB) {
        for (int i = 0; i < count; i++) pb[i] = cur[i];
        isPB = false;
    }
    save_data();
}

static void clear_run_arrays(void) {
    for (int i = 0; i < MAX_SPLITS; i++) { cur[i] = 0; goldHit[i] = false; }
}

static void reset_run(void) {
    commit_run();
    state = STATE_IDLE;
    idx = 0; finalTime = 0; isPB = false;
    clear_run_arrays();
}

static void start_run(void) {
    state = STATE_RUNNING;
    idx = 0;
    startTime = (s64)osGetTime();
}

static void do_split(void) {
    s64 t = (s64)osGetTime() - startTime;
    cur[idx] = t;
    s64 seg = t - (idx > 0 ? cur[idx - 1] : 0);
    if (gold[idx] < 0) gold[idx] = seg;
    else if (seg < gold[idx]) { gold[idx] = seg; goldHit[idx] = true; }
    idx++;
    if (idx >= count) {
        state = STATE_FINISHED;
        finalTime = t;
        isPB = (pb[count - 1] < 0 || t < pb[count - 1]);
        save_data();
    }
}

static void split_action(void) {
    if (state == STATE_IDLE) start_run();
    else if (state == STATE_RUNNING) do_split();
}

static void request_reset(void) {
    if (state == STATE_RUNNING) {
        if (confirmReset) modal = M_RESET_RUN;
        else reset_run();
    } else if (state == STATE_FINISHED) {
        reset_run();
    }
}

static bool load_game(int i) {
    if (i < 0 || i >= gameCount) return false;
    snprintf(gameDir, sizeof gameDir, GAMES_DIR "/%s", games[i].id);
    snprintf(gameName, sizeof gameName, "%s", games[i].name);
    snprintf(gameCat, sizeof gameCat, "%s", games[i].cat);
    load_splits();
    if (count == 0) { show_toast("That game has no splits."); return false; }
    load_data();
    clear_run_arrays();
    state = STATE_IDLE; idx = 0; finalTime = 0; isPB = false;
    gameLoaded = true;
    screen = SCR_RUN;
    return true;
}

static void leave_game(void) {
    if (gameLoaded) reset_run();
    gameLoaded = false;
    scan_games();
    screen = SCR_WELCOME;
}

static void reset_game_progress(void) {
    clear_data();
    save_data();
    state = STATE_IDLE; idx = 0; finalTime = 0; isPB = false;
    clear_run_arrays();
}

static void reset_all_progress(void) {
    for (int i = 0; i < gameCount; i++) {
        char p[160];
        game_path(games[i].id, "data.txt", p, sizeof p);
        remove(p);
    }
    if (gameLoaded) {
        clear_data();
        state = STATE_IDLE; idx = 0; finalTime = 0; isPB = false;
        clear_run_arrays();
    }
}

// ---------- drawing helpers ----------

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

static C2D_TextBuf textBuf;
static float digitW = 14.0f, colonW = 6.0f, dotW = 6.0f;

static void draw_text(const char *s, float x, float y, float scale,
                      u32 color, int align, float maxW) {
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, textBuf, s);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    if (maxW > 0 && w > maxW) { scale *= maxW / w; w = maxW; }
    if (align == ALIGN_CENTER)     x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;
    C2D_DrawText(&t, C2D_WithColor, x, y, Z, scale, scale, color);
}

static float text_width(const char *s, float scale) {
    C2D_Text t;
    float w, h;
    C2D_TextParse(&t, textBuf, s);
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

static void measure_timer_font(void) {
    char one[2] = {0, 0};
    float maxd = 0;
    for (char c = '0'; c <= '9'; c++) {
        one[0] = c;
        float w = text_width(one, 1.0f);
        if (w > maxd) maxd = w;
    }
    if (maxd > 0) digitW = maxd;
    colonW = text_width(":", 1.0f);
    dotW   = text_width(".", 1.0f);
    C2D_TextBufClear(textBuf);
}

static float glyph_advance(char c) {
    if (c == ':') return colonW;
    if (c == '.') return dotW;
    return digitW;
}

static void draw_timer(const char *str, float cx, float y, float scale, u32 color) {
    float total = 0;
    for (const char *p = str; *p; p++) total += glyph_advance(*p);
    total *= scale;
    if (total > 370.0f) { scale *= 370.0f / total; total = 370.0f; }
    float x = cx - total / 2;
    char one[2] = {0, 0};
    for (const char *p = str; *p; p++) {
        float adv = glyph_advance(*p) * scale;
        one[0] = *p;
        draw_text(one, x + adv / 2, y, scale, color, ALIGN_CENTER, 0);
        x += adv;
    }
}

static void draw_ghost(float cx, float cy, float s) {
    u32 body = C_TEXT, eye = C2D_Color32(20, 46, 32, 255);
    C2D_DrawCircleSolid(cx, cy - 2*s, Z, 9*s, body);
    C2D_DrawRectSolid(cx - 9*s, cy - 2*s, Z, 18*s, 10*s, body);
    C2D_DrawCircleSolid(cx - 6*s, cy + 8*s, Z, 3*s, body);
    C2D_DrawCircleSolid(cx,       cy + 8*s, Z, 3*s, body);
    C2D_DrawCircleSolid(cx + 6*s, cy + 8*s, Z, 3*s, body);
    C2D_DrawCircleSolid(cx - 3.5f*s, cy - 3*s, Z, 1.8f*s, eye);
    C2D_DrawCircleSolid(cx + 3.5f*s, cy - 3*s, Z, 1.8f*s, eye);
}

static void draw_gear(float cx, float cy, u32 col, u32 hole) {
    for (int i = 0; i < 4; i++) {
        float a = i * 0.7853982f;
        float dx = cosf(a) * 8.0f, dy = sinf(a) * 8.0f;
        C2D_DrawCircleSolid(cx + dx, cy + dy, Z, 2.2f, col);
        C2D_DrawCircleSolid(cx - dx, cy - dy, Z, 2.2f, col);
    }
    C2D_DrawCircleSolid(cx, cy, Z, 7, col);
    C2D_DrawCircleSolid(cx, cy, Z, 3, hole);
}

static void draw_bg(float w) {
    C2D_DrawRectangle(0, 0, Z, w, 240, C_BG_TOP, C_BG_TOP, C_BG_BOT, C_BG_BOT);
}

static void draw_title_bar(const char *title, const char *pill) {
    C2D_DrawRectSolid(0, 0, Z, 400, 30, C_PANEL);
    C2D_DrawRectSolid(0, 30, Z, 400, 2, C_GREEN);
    draw_ghost(22, 15, 1.0f);
    float pillX = 396;
    if (pill && pill[0]) {
        float cw = text_width(pill, 0.5f);
        if (cw > 90) cw = 90;
        pillX = 388 - (cw + 16);
        C2D_DrawRectSolid(pillX, 6, Z, cw + 16, 18, C_PURPLE_DK);
        draw_text(pill, pillX + (cw + 16) / 2, 8, 0.5f, C_TEXT, ALIGN_CENTER, 90);
    }
    draw_text(title, 40, 6, 0.55f, C_TEXT, ALIGN_LEFT, pillX - 48);
}

static void button(float x, float y, float w, float h, const char *label,
                   u32 fill, u32 textCol, bool enabled) {
    C2D_DrawRectSolid(x, y, Z, w, h, enabled ? fill : C_ROW);
    C2D_DrawRectSolid(x, y + h - 2, Z, w, 2, enabled ? C_PURPLE : C_PANEL);
    draw_text(label, x + w / 2, y + (h - 17.0f) / 2, 0.55f,
              enabled ? textCol : C_DIM, ALIGN_CENTER, w - 8);
}

static bool in_rect(touchPosition t, float x, float y, float w, float h) {
    return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

// ---------- screens: top ----------

static void draw_top_run(void) {
    char buf[40], line[96];
    s64 elapsed = 0;
    if (state == STATE_RUNNING)       elapsed = (s64)osGetTime() - startTime;
    else if (state == STATE_FINISHED) elapsed = finalTime;

    draw_bg(400);
    draw_title_bar(gameName, gameCat);

    fmt_time(buf, sizeof buf, elapsed);
    u32 timerCol = C_TEXT;
    if (state == STATE_IDLE) timerCol = C_DIM;
    if (state == STATE_FINISHED && isPB) timerCol = C_GOLD;
    draw_timer(buf, 200, 46, 2.0f, timerCol);

    if (state == STATE_IDLE) {
        draw_text("READY", 200, 124, 0.5f, C_PURPLE, ALIGN_CENTER, 0);
        draw_text(count > 0 ? names[0] : "", 200, 138, 0.85f, C_TEXT, ALIGN_CENTER, 340);
        draw_text("Tap SPLIT or press A to start", 200, 172, 0.65f, C_DIM, ALIGN_CENTER, 0);
    } else if (state == STATE_RUNNING) {
        snprintf(line, sizeof line, "SPLIT %d OF %d", idx + 1, count);
        draw_text(line, 200, 124, 0.5f, C_PURPLE, ALIGN_CENTER, 0);
        draw_text(names[idx], 200, 138, 0.85f, C_TEXT, ALIGN_CENTER, 340);
        if (pb[idx] >= 0) {
            fmt_delta(buf, sizeof buf, elapsed - pb[idx]);
            snprintf(line, sizeof line, "vs PB  %s", buf);
            draw_text(line, 200, 172, 0.85f,
                      (elapsed - pb[idx]) < 0 ? C_AHEAD : C_BEHIND, ALIGN_CENTER, 0);
        } else {
            draw_text("No PB yet", 200, 172, 0.7f, C_DIM, ALIGN_CENTER, 0);
        }
    } else {
        draw_text("RUN COMPLETE", 200, 124, 0.5f, C_PURPLE, ALIGN_CENTER, 0);
        draw_text(isPB ? "NEW PERSONAL BEST!" : "Nice run!", 200, 138, 0.85f,
                  isPB ? C_GOLD : C_TEXT, ALIGN_CENTER, 340);
        if (count > 0 && pb[count - 1] >= 0) {
            fmt_delta(buf, sizeof buf, finalTime - pb[count - 1]);
            snprintf(line, sizeof line, "vs PB  %s", buf);
            draw_text(line, 200, 172, 0.85f,
                      (finalTime - pb[count - 1]) < 0 ? C_AHEAD : C_BEHIND, ALIGN_CENTER, 0);
        }
    }

    if (count > 0) {
        float margin = 20, gap = 4;
        float w = (400 - 2 * margin - gap * (count - 1)) / count;
        for (int i = 0; i < count; i++) {
            u32 c = C_PANEL_LT;
            if (i < idx)                                 c = goldHit[i] ? C_GOLD : C_GREEN;
            else if (i == idx && state == STATE_RUNNING) c = C_PURPLE;
            C2D_DrawRectSolid(margin + i * (w + gap), 214, Z, w, 10, c);
        }
    }
}

static void draw_top_welcome(void) {
    draw_bg(400);
    draw_ghost(200, 78, 4.0f);
    draw_text("SPEEDRUN TIMER", 200, 128, 1.0f, C_TEXT, ALIGN_CENTER, 360);
    draw_text(gameCount > 0 ? "Pick a game on the bottom screen"
                            : "Create your first game to begin",
              200, 172, 0.6f, C_DIM, ALIGN_CENTER, 360);
    C2D_DrawRectSolid(150, 164, Z, 100, 2, C_GREEN);
}

static void draw_top_settings(void) {
    draw_bg(400);
    draw_title_bar("Settings", gameLoaded ? gameCat : "");
    draw_text("SETTINGS", 200, 70, 1.1f, C_TEXT, ALIGN_CENTER, 360);
    C2D_DrawRectSolid(150, 112, Z, 100, 2, C_GREEN);
    if (gameLoaded) draw_text(gameName, 200, 126, 0.65f, C_DIM, ALIGN_CENTER, 360);
    draw_text(audioStatus, 200, 156, 0.55f, audioOK ? C_PURPLE : C_BEHIND, ALIGN_CENTER, 370);
    draw_text("Put your own music.wav in /3ds/speedtimer/", 200, 178, 0.45f, C_DIM, ALIGN_CENTER, 370);
}

// ---------- screens: bottom ----------

// run screen layout (shared with input handling)
#define RUN_SPLIT_X   8
#define RUN_SPLIT_W   128
#define RUN_RESET_X   142
#define RUN_RESET_W   82
#define RUN_EXIT_X    230
#define RUN_EXIT_W    82
#define RUN_BTN_Y     200
#define RUN_BTN_H     34
#define GEAR_X 284
#define GEAR_W 36

static void draw_bottom_run(void) {
    char buf[40];
    draw_bg(320);

    C2D_DrawRectSolid(0, 0, Z, 320, 28, C_PANEL);
    C2D_DrawRectSolid(0, 28, Z, 320, 2, C_GREEN);
    draw_text("SPLITS", 12, 5, 0.6f, C_GREEN, ALIGN_LEFT, 0);
    fmt_time(buf, sizeof buf, count > 0 ? pb[count - 1] : -1);
    char pbLine[60];
    snprintf(pbLine, sizeof pbLine, "PB  %s", buf);
    draw_text(pbLine, 276, 6, 0.55f, C_DIM, ALIGN_RIGHT, 0);
    draw_gear(302, 14, C_PURPLE, C_PANEL);

    const int rowH = 22, listTop = 34, visible = 7;
    int focus = idx < count ? idx : count - 1;
    int first = 0;
    if (count > visible) {
        first = focus - 3;
        if (first < 0) first = 0;
        if (first > count - visible) first = count - visible;
    }
    for (int r = 0; r < visible; r++) {
        int i = first + r;
        if (i >= count) break;
        float y = listTop + r * rowH;
        bool current = (state == STATE_RUNNING && i == idx);
        C2D_DrawRectSolid(0, y, Z, 310, rowH, current ? C_PANEL_LT : (r % 2 ? C_ROW_ALT : C_ROW));
        if (current) C2D_DrawRectSolid(0, y, Z, 4, rowH, C_GREEN);
        bool done = (i < idx);
        draw_text(names[i], 12, y + 3, 0.55f, (done || current) ? C_TEXT : C_DIM, ALIGN_LEFT, 135);
        if (done) {
            fmt_time(buf, sizeof buf, cur[i]);
            draw_text(buf, 304, y + 3, 0.55f, C_TEXT, ALIGN_RIGHT, 0);
            if (pb[i] >= 0) {
                char db[40];
                fmt_delta(db, sizeof db, cur[i] - pb[i]);
                u32 dc = (cur[i] - pb[i]) < 0 ? C_AHEAD : C_BEHIND;
                if (goldHit[i]) dc = C_GOLD;
                draw_text(db, 232, y + 3, 0.55f, dc, ALIGN_RIGHT, 0);
            }
        } else {
            fmt_time(buf, sizeof buf, pb[i]);
            draw_text(buf, 304, y + 3, 0.55f, C_DIM, ALIGN_RIGHT, 0);
        }
    }
    if (count > visible) {
        float trackH = visible * rowH;
        float thumbH = trackH * visible / count;
        float thumbY = listTop + (trackH - thumbH) * first / (count - visible);
        C2D_DrawRectSolid(313, listTop, Z, 4, trackH, C_ROW);
        C2D_DrawRectSolid(313, thumbY, Z, 4, thumbH, C_PURPLE);
    }

    const char *splitLabel = state == STATE_IDLE ? "START" :
                             state == STATE_RUNNING ? "SPLIT" : "DONE";
    button(RUN_SPLIT_X, RUN_BTN_Y, RUN_SPLIT_W, RUN_BTN_H, splitLabel,
           C_GREEN, C_GREEN_DK, state != STATE_FINISHED);
    button(RUN_RESET_X, RUN_BTN_Y, RUN_RESET_W, RUN_BTN_H, "RESET",
           C_PURPLE_DK, C_TEXT, state != STATE_IDLE);
    button(RUN_EXIT_X, RUN_BTN_Y, RUN_EXIT_W, RUN_BTN_H, "Save & Exit",
           C_PANEL, C_TEXT, true);
}

// welcome layout
#define W_ROW_H   30
#define W_VISIBLE 5
#define W_TOP     34

static void draw_triangle_btn(float x, float y, bool up, bool enabled) {
    C2D_DrawRectSolid(x, y, Z, 32, 32, enabled ? C_PANEL : C_ROW);
    u32 c = enabled ? C_PURPLE : C_PANEL;
    if (up) C2D_DrawTriangle(x + 16, y + 9, c, x + 7, y + 23, c, x + 25, y + 23, c, Z);
    else    C2D_DrawTriangle(x + 7, y + 9, c, x + 25, y + 9, c, x + 16, y + 23, c, Z);
}

static void draw_bottom_welcome(void) {
    draw_bg(320);
    C2D_DrawRectSolid(0, 0, Z, 320, 28, C_PANEL);
    C2D_DrawRectSolid(0, 28, Z, 320, 2, C_GREEN);
    draw_text("YOUR GAMES", 12, 5, 0.6f, C_GREEN, ALIGN_LEFT, 0);
    draw_gear(302, 14, C_PURPLE, C_PANEL);

    if (gameCount == 0) {
        draw_text("No games yet.", 160, 80, 0.7f, C_TEXT, ALIGN_CENTER, 0);
        draw_text("Tap NEW GAME to create one.", 160, 108, 0.55f, C_DIM, ALIGN_CENTER, 0);
    }
    for (int r = 0; r < W_VISIBLE; r++) {
        int i = listFirst + r;
        if (i >= gameCount) break;
        float y = W_TOP + r * W_ROW_H;
        bool sel = (i == selGame);
        C2D_DrawRectSolid(0, y, Z, 280, W_ROW_H - 2, sel ? C_PANEL_LT : (r % 2 ? C_ROW_ALT : C_ROW));
        if (sel) C2D_DrawRectSolid(0, y, Z, 4, W_ROW_H - 2, C_GREEN);
        draw_text(games[i].name, 12, y + 5, 0.6f, C_TEXT, ALIGN_LEFT, 170);
        draw_text(games[i].cat, 272, y + 7, 0.5f, C_PURPLE, ALIGN_RIGHT, 85);
    }
    draw_triangle_btn(284, W_TOP, true, listFirst > 0);
    draw_triangle_btn(284, W_TOP + W_VISIBLE * W_ROW_H - 32, false, listFirst + W_VISIBLE < gameCount);

    button(8, 200, 196, 34, "+ NEW GAME", C_GREEN, C_GREEN_DK, true);
    button(212, 200, 100, 34, "Exit", C_PANEL, C_TEXT, true);
}

#define S_ROWS 7
#define S_TOP 34
#define S_ROW_H 24

static const char *settings_label(int i) {
    switch (i) {
        case 0: return "Confirm before reset";
        case 1: return "Music";
        case 2: return "Music volume";
        case 3: return "Reset this game's progress";
        case 4: return "Reset ALL games' progress";
        case 5: return "Switch game";
        default: return "Delete this game";
    }
}

static bool settings_enabled(int i) {
    if (i == 1 || i == 2) return audioOK;
    if (i == 3 || i == 5 || i == 6) return gameLoaded;
    return true;
}

static void draw_bottom_settings(void) {
    char buf[24];
    draw_bg(320);
    C2D_DrawRectSolid(0, 0, Z, 320, 28, C_PANEL);
    C2D_DrawRectSolid(0, 28, Z, 320, 2, C_GREEN);
    draw_text("SETTINGS", 12, 5, 0.6f, C_GREEN, ALIGN_LEFT, 0);

    for (int i = 0; i < S_ROWS; i++) {
        float y = S_TOP + i * S_ROW_H;
        bool en = settings_enabled(i);
        C2D_DrawRectSolid(0, y, Z, 320, S_ROW_H - 2, i % 2 ? C_ROW_ALT : C_ROW);
        u32 lc = en ? ((i == 4 || i == 6) ? C_BEHIND : C_TEXT) : C_DIM;
        draw_text(settings_label(i), 12, y + 3, 0.55f, lc, ALIGN_LEFT, 200);
        const char *val = ">"; u32 vc = C_PURPLE;
        if (i == 0) { val = confirmReset ? "ON" : "OFF"; vc = confirmReset ? C_AHEAD : C_DIM; }
        if (i == 1) { val = !audioOK ? "N/A" : (musicOn ? "ON" : "OFF"); vc = (audioOK && musicOn) ? C_AHEAD : C_DIM; }
        if (i == 2) { snprintf(buf, sizeof buf, "%d%%", volLevels[volIdx]); val = buf; vc = audioOK ? C_GOLD : C_DIM; }
        if (!en && i > 2) vc = C_DIM;
        draw_text(val, 308, y + 3, 0.55f, vc, ALIGN_RIGHT, 0);
    }
    button(8, 208, 304, 28, "BACK", C_PURPLE_DK, C_TEXT, true);
}

static void draw_modal(void) {
    const char *title = "", *body = "";
    switch (modal) {
        case M_RESET_RUN:   title = "Reset this run?";        body = "Your current run will end."; break;
        case M_RESET_GAME:  title = "Reset this game?";       body = "Deletes your PB and gold splits."; break;
        case M_RESET_ALL:   title = "Reset ALL games?";       body = "Deletes PB and golds for every game."; break;
        case M_DELETE_GAME: title = "Delete this game?";      body = "Removes its splits and saved times."; break;
        default: return;
    }
    C2D_DrawRectSolid(0, 0, Z, 320, 240, C_SHADE);
    C2D_DrawRectSolid(24, 56, Z, 272, 138, C_PANEL);
    C2D_DrawRectSolid(24, 56, Z, 272, 3, C_GREEN);
    draw_text(title, 160, 68, 0.75f, C_TEXT, ALIGN_CENTER, 250);
    draw_text(body, 160, 100, 0.5f, C_DIM, ALIGN_CENTER, 250);
    button(44, 140, 100, 36, "YES  (A)", C_BEHIND, C_GREEN_DK, true);
    button(176, 140, 100, 36, "NO  (B)", C_PURPLE_DK, C_TEXT, true);
}

static void render(C3D_RenderTarget *topT, C3D_RenderTarget *botT) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(textBuf);

    C2D_TargetClear(topT, C_BG_TOP);
    C2D_SceneBegin(topT);
    if (screen == SCR_RUN) draw_top_run();
    else if (screen == SCR_SETTINGS) draw_top_settings();
    else draw_top_welcome();
    if (toastFrames > 0) {
        C2D_DrawRectSolid(0, 204, Z, 400, 28, C_PURPLE_DK);
        draw_text(toast, 200, 208, 0.55f, C_TEXT, ALIGN_CENTER, 380);
    }

    C2D_TargetClear(botT, C_BG_TOP);
    C2D_SceneBegin(botT);
    if (screen == SCR_RUN) draw_bottom_run();
    else if (screen == SCR_SETTINGS) draw_bottom_settings();
    else draw_bottom_welcome();
    draw_modal();

    C3D_FrameEnd(0);
}

// ---------- creating a game (uses the 3DS keyboard) ----------

static C3D_RenderTarget *gTop, *gBot;

static bool ask_text(const char *hint, const char *initial, char *out, size_t n,
                     int maxLen, const char *leftLabel, const char *rightLabel) {
    SwkbdState s;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, maxLen);
    swkbdSetValidation(&s, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetHintText(&s, hint);
    if (initial) swkbdSetInitialText(&s, initial);
    swkbdSetButton(&s, SWKBD_BUTTON_LEFT, leftLabel, false);
    swkbdSetButton(&s, SWKBD_BUTTON_RIGHT, rightLabel, true);
    out[0] = 0;
    SwkbdButton b = swkbdInputText(&s, out, n);
    return b == SWKBD_BUTTON_RIGHT;
}

static void create_game_flow(void) {
    char name[40], cat[28], sp[40];
    if (gameCount >= MAX_GAMES) { show_toast("Game limit reached."); return; }
    render(gTop, gBot);
    if (!ask_text("Game name (e.g. Luigi's Mansion 3)", NULL, name, sizeof name, 28, "Cancel", "Next")) return;
    render(gTop, gBot);
    if (!ask_text("Category (e.g. Any%)", "Any%", cat, sizeof cat, 20, "Cancel", "Next")) return;

    char splitsBuf[MAX_SPLITS][NAME_LEN];
    int n = 0;
    while (n < MAX_SPLITS) {
        char hint[64];
        snprintf(hint, sizeof hint, "Split %d name  (Done when finished)", n + 1);
        render(gTop, gBot);
        if (!ask_text(hint, NULL, sp, sizeof sp, NAME_LEN - 1, "Done", "Add")) break;
        snprintf(splitsBuf[n], NAME_LEN, "%s", sp);
        n++;
    }
    if (n == 0) { show_toast("A game needs at least one split."); return; }

    char id[48];
    mkdir("sdmc:/3ds", 0777); mkdir(DIR_PATH, 0777); mkdir(GAMES_DIR, 0777);
    make_id(name, id, sizeof id);
    char dir[160];
    snprintf(dir, sizeof dir, GAMES_DIR "/%s", id);
    mkdir(dir, 0777);
    char p[200];
    game_path(id, "game.txt", p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { show_toast("Couldn't write to the SD card."); return; }
    fprintf(f, "%s\n%s\n", name, cat);
    fclose(f);
    game_path(id, "splits.txt", p, sizeof p);
    f = fopen(p, "w");
    if (f) { for (int i = 0; i < n; i++) fprintf(f, "%s\n", splitsBuf[i]); fclose(f); }

    scan_games();
    for (int i = 0; i < gameCount; i++) if (!strcmp(games[i].id, id)) selGame = i;
    if (selGame < listFirst) listFirst = selGame;
    if (selGame >= listFirst + W_VISIBLE) listFirst = selGame - W_VISIBLE + 1;
    show_toast("Game created!");
}

// ---------- input ----------

static void select_move(int d) {
    if (gameCount == 0) return;
    selGame += d;
    if (selGame < 0) selGame = 0;
    if (selGame >= gameCount) selGame = gameCount - 1;
    if (selGame < listFirst) listFirst = selGame;
    if (selGame >= listFirst + W_VISIBLE) listFirst = selGame - W_VISIBLE + 1;
}

static void open_settings(void) {
    settingsFrom = screen;
    screen = SCR_SETTINGS;
}

static void do_modal_yes(void) {
    switch (modal) {
        case M_RESET_RUN:   reset_run(); break;
        case M_RESET_GAME:  reset_game_progress(); show_toast("Progress reset for this game."); break;
        case M_RESET_ALL:   reset_all_progress(); show_toast("All progress reset."); break;
        case M_DELETE_GAME: {
            char id[48];
            for (int i = 0; i < gameCount; i++) {
                char dir[160]; snprintf(dir, sizeof dir, GAMES_DIR "/%s", games[i].id);
                if (!strcmp(dir, gameDir)) snprintf(id, sizeof id, "%s", games[i].id);
            }
            gameLoaded = false;
            delete_game_files(id);
            scan_games();
            screen = SCR_WELCOME;
            show_toast("Game deleted.");
            break;
        }
        default: break;
    }
}

static void settings_activate(int i) {
    if (!settings_enabled(i)) return;
    switch (i) {
        case 0: confirmReset = !confirmReset; save_settings(); break;
        case 1: musicOn = !musicOn; audio_apply(); save_settings(); break;
        case 2: volIdx = (volIdx + 1) % 4; audio_apply(); save_settings(); break;
        case 3: modal = M_RESET_GAME; break;
        case 4: modal = M_RESET_ALL; break;
        case 5: leave_game(); break;
        case 6: modal = M_DELETE_GAME; break;
    }
}

static void handle_input(u32 kd, bool tap, touchPosition tp) {
    if (modal != M_NONE) {
        bool yes = kd & KEY_A, no = kd & KEY_B;
        if (tap) {
            if (in_rect(tp, 44, 140, 100, 36)) yes = true;
            else if (in_rect(tp, 176, 140, 100, 36)) no = true;
        }
        if (yes) { Modal m = modal; do_modal_yes(); if (modal == m) modal = M_NONE; }
        else if (no) modal = M_NONE;
        return;
    }

    if (screen == SCR_WELCOME) {
        if (kd & KEY_START) quit = true;
        if (kd & KEY_DUP) select_move(-1);
        if (kd & KEY_DDOWN) select_move(1);
        if (kd & KEY_X) open_settings();
        if ((kd & KEY_A) && gameCount > 0) load_game(selGame);
        if (tap) {
            if (in_rect(tp, GEAR_X, 0, GEAR_W, 28)) open_settings();
            else if (in_rect(tp, 8, 200, 196, 34)) create_game_flow();
            else if (in_rect(tp, 212, 200, 100, 34)) quit = true;
            else if (in_rect(tp, 284, W_TOP, 32, 32)) { if (listFirst > 0) { listFirst--; if (selGame >= listFirst + W_VISIBLE) selGame = listFirst + W_VISIBLE - 1; } }
            else if (in_rect(tp, 284, W_TOP + W_VISIBLE * W_ROW_H - 32, 32, 32)) {
                if (listFirst + W_VISIBLE < gameCount) { listFirst++; if (selGame < listFirst) selGame = listFirst; }
            }
            else if (in_rect(tp, 0, W_TOP, 280, W_VISIBLE * W_ROW_H)) {
                int r = (int)((tp.py - W_TOP) / W_ROW_H);
                int i = listFirst + r;
                if (i < gameCount) { selGame = i; load_game(i); }
            }
        }
        return;
    }

    if (screen == SCR_SETTINGS) {
        if (kd & KEY_B) screen = settingsFrom;
        if (tap) {
            if (in_rect(tp, 8, 208, 304, 28)) screen = settingsFrom;
            else if (in_rect(tp, 0, S_TOP, 320, S_ROWS * S_ROW_H)) {
                settings_activate((int)((tp.py - S_TOP) / S_ROW_H));
            }
        }
        return;
    }

    // SCR_RUN
    if (kd & KEY_START) { commit_run(); quit = true; }
    if (kd & KEY_SELECT) request_reset();
    if (kd & KEY_A) split_action();
    if (kd & KEY_X) open_settings();
    if (tap) {
        if (in_rect(tp, GEAR_X, 0, GEAR_W, 28)) open_settings();
        else if (in_rect(tp, RUN_SPLIT_X, RUN_BTN_Y, RUN_SPLIT_W, RUN_BTN_H)) split_action();
        else if (in_rect(tp, RUN_RESET_X, RUN_BTN_Y, RUN_RESET_W, RUN_BTN_H)) request_reset();
        else if (in_rect(tp, RUN_EXIT_X, RUN_BTN_Y, RUN_EXIT_W, RUN_BTN_H)) { commit_run(); quit = true; }
    }
}

// ---------- main ----------

int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    gTop = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    gBot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(4096);
    measure_timer_font();

    mkdir("sdmc:/3ds", 0777);
    mkdir(DIR_PATH, 0777);
    mkdir(GAMES_DIR, 0777);
    load_settings();
    clear_data();
    clear_run_arrays();
    migrate_old();
    scan_games();
    audio_init();
    if (gameCount == 0 && !audioOK) show_toast(audioStatus);

    while (aptMainLoop() && !quit) {
        hidScanInput();
        u32 kd = hidKeysDown();
        touchPosition tp = {0, 0};
        bool tap = false;
        if (kd & KEY_TOUCH) { hidTouchRead(&tp); tap = true; }

        handle_input(kd, tap, tp);
        if (toastFrames > 0) toastFrames--;
        if (quit) break;
        render(gTop, gBot);
    }

    commit_run();
    audio_exit();
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
