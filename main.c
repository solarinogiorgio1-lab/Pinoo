// PSP EQ - lettore WAV con equalizzatore a 6 bande (stile Spotify)
// File audio: ms0:/test.wav  (WAV PCM 16 bit, stereo, 44100 Hz)
// Controlli: Sinistra/Destra = banda, Su/Giu = +/- 1 dB, START = esci

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

PSP_MODULE_INFO("PSPEQ", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define WAV_PATH    "ms0:/test.wav"
#define SAMPLE_RATE 44100.0f
#define BLOCK       1024     // frame per blocco (deve essere multiplo di 64)
#define BANDS       6
#define Q_FACTOR    1.0f
#define PREAMP      0.7f     // abbassa il volume per evitare distorsione se alzi i bassi
#define GAIN_MAX    12

static const float FREQ[BANDS] = { 60, 150, 400, 1000, 2400, 15000 };
static float gain_db[BANDS];

// ---------- Filtro biquad "peaking" ----------

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} Biquad;

static Biquad filt[BANDS][2];   // [banda][canale sinistro/destro]

static void set_band(int b) {
    float A     = powf(10.0f, gain_db[b] / 40.0f);
    float w0    = 2.0f * 3.14159265f * FREQ[b] / SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * Q_FACTOR);
    float c     = cosf(w0);
    float a0    = 1.0f + alpha / A;

    for (int ch = 0; ch < 2; ch++) {
        Biquad *f = &filt[b][ch];
        f->b0 = (1.0f + alpha * A) / a0;
        f->b1 = (-2.0f * c) / a0;
        f->b2 = (1.0f - alpha * A) / a0;
        f->a1 = (-2.0f * c) / a0;
        f->a2 = (1.0f - alpha / A) / a0;
    }
}

static inline float run(Biquad *f, float x) {
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
            - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;  f->x1 = x;
    f->y2 = f->y1;  f->y1 = y;
    return y;
}

// ---------- Buffer audio ----------

static short buf[BLOCK * 2] __attribute__((aligned(64)));

static void process(void) {
    for (int i = 0; i < BLOCK; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float s = (buf[i * 2 + ch] / 32768.0f) * PREAMP;
            for (int b = 0; b < BANDS; b++)
                s = run(&filt[b][ch], s);
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            buf[i * 2 + ch] = (short)(s * 32767.0f);
        }
    }
}

// ---------- Lettura WAV ----------

typedef struct { long data_start; uint32_t data_size; } WavInfo;

static int open_wav(FILE *f, WavInfo *w) {
    char id[4];
    uint32_t size, rate = 0;
    uint16_t fmt = 0, ch = 0, bits = 0;

    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) return 0;
    fseek(f, 8, SEEK_SET);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) return 0;

    while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            fread(&fmt, 2, 1, f);
            fread(&ch, 2, 1, f);
            fread(&rate, 4, 1, f);
            fseek(f, 6, SEEK_CUR);                 // byte rate + block align
            fread(&bits, 2, 1, f);
            fseek(f, (long)size - 16 + (size & 1), SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            w->data_start = ftell(f);
            w->data_size  = size;
            return fmt == 1 && ch == 2 && bits == 16 && rate == 44100;
        } else {
            fseek(f, (long)size + (size & 1), SEEK_CUR);
        }
    }
    return 0;
}

// ---------- Schermo ----------

static void draw(int sel) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("PSP EQ  -  START = esci\n\n");
    for (int b = 0; b < BANDS; b++)
        pspDebugScreenPrintf("%c %5d Hz   %+3d dB   \n",
                             b == sel ? '>' : ' ', (int)FREQ[b], (int)gain_db[b]);
    pspDebugScreenPrintf("\nSinistra/Destra = banda\nSu/Giu = +/- dB\n");
}

// ---------- Uscita col tasto HOME ----------

static int exit_callback(int a, int b, void *c) {
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cb = sceKernelCreateCallback("exit_cb", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int th = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

// ---------- Main ----------

int main(void) {
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    WavInfo w;
    FILE *f = fopen(WAV_PATH, "rb");
    if (!f || !open_wav(f, &w)) {
        pspDebugScreenPrintf("Metti ms0:/test.wav\n(PCM 16 bit, stereo, 44100 Hz)\n");
        sceKernelSleepThread();
        return 0;
    }

    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK, PSP_AUDIO_FORMAT_STEREO);

    for (int b = 0; b < BANDS; b++) {
        gain_db[b] = 0;
        set_band(b);
    }

    int sel = 0;
    unsigned int prev = 0;
    uint32_t left = w.data_size;
    fseek(f, w.data_start, SEEK_SET);
    draw(sel);

    while (1) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        int changed = 0;
        if (pressed & PSP_CTRL_LEFT)  { sel = (sel + BANDS - 1) % BANDS; changed = 1; }
        if (pressed & PSP_CTRL_RIGHT) { sel = (sel + 1) % BANDS;         changed = 1; }
        if ((pressed & PSP_CTRL_UP) && gain_db[sel] < GAIN_MAX) {
            gain_db[sel] += 1; set_band(sel); changed = 1;
        }
        if ((pressed & PSP_CTRL_DOWN) && gain_db[sel] > -GAIN_MAX) {
            gain_db[sel] -= 1; set_band(sel); changed = 1;
        }
        if (changed) draw(sel);
        if (pressed & PSP_CTRL_START) break;

        // leggi un blocco dal file (il resto resta silenzio)
        memset(buf, 0, sizeof(buf));
        size_t want = BLOCK * 4;                  // 2 canali * 2 byte
        if (want > left) want = left;
        size_t got = fread(buf, 1, want, f);
        left -= got;
        if (got < want || left == 0) {            // fine brano: ricomincia
            fseek(f, w.data_start, SEEK_SET);
            left = w.data_size;
        }

        process();
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, buf);
    }

    sceAudioChRelease(ch);
    fclose(f);
    sceKernelExitGame();
    return 0;
}
