/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * host_test.c: the harness that tools/host_test.sh builds against the library for the host.
 *
 *   host_test gen OUT.raw              writes the synthetic test signal (8 kHz, 16-bit, mono)
 *   host_test enc MODE IN.raw OUT.bit  encodes it, frame after frame
 *   host_test dec MODE IN.bit OUT.raw  decodes it, with the LPC post filter MeshKode uses
 *   host_test cmp A.raw B.raw          says how two signals differ (samples, max error, SNR)
 *
 * The signal is made with integer arithmetic only, so it is the same on every host; the codec itself is float and
 * uses libm, so its output is only comparable between builds on the same host with the same flags.
 *
 * Codec 2 keeps one random generator for the whole process (codec2_rand in sine.c), shared by every decoder: run one
 * mode per process, as host_test.sh does, or a decode depends on what was decoded before it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "codec2.h"

#define FS          8000          /* sample rate */
#define SECONDS     6             /* length of the test signal */
#define NSAMPLES    (FS * SECONDS)

/* MeshKode's (and Meshtastic AudioModule's) post filter. */
#define PF_ON    1
#define PF_BASS  0
#define PF_BETA  0.8f
#define PF_GAMMA 0.2f

/* ------------------------------------------------------------------------------------------------ the signal */

static uint32_t s_lcg = 0x12345678u;

static int32_t noise(void) /* -32768..32767 */
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return (int32_t)(s_lcg >> 16) - 32768;
}

/* A two-pole resonator in Q14: y = g*x + a1*y1 - a2*y2. */
typedef struct {
    int32_t a1, a2, g;
    int32_t y1, y2;
} reso_t;

static int32_t reso_run(reso_t * r, int32_t x)
{
    int64_t acc = (int64_t)r->g * x + (int64_t)r->a1 * r->y1 - (int64_t)r->a2 * r->y2;
    int32_t y = (int32_t)(acc / 16384);
    if ( y > 2000000 ) y = 2000000;
    if ( y < -2000000 ) y = -2000000;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* Formant pairs: (a1, a2, g) in Q14 for F1 and F2, precomputed for 8 kHz. */
typedef struct {
    int32_t f1[3], f2[3];
} vowel_t;

static const vowel_t s_vowels[] = {
    { { 26969, 15266, 569 }, { 18446, 15028, 693 } }, /* 700 Hz, 1200 Hz: "a" */
    { { 30877, 15386, 507 }, { -7212, 14563, 937 } }, /* 300 Hz, 2300 Hz: "i" */
    { { 29337, 15386, 507 }, { 7297, 14910, 754 } },  /* 500 Hz, 1700 Hz: "e" */
    { { 30877, 15386, 507 }, { -13970, 14449, 998 } } /* 300 Hz, 2600 Hz */
};

/* One segment of the script. */
typedef enum { SEG_SILENCE, SEG_VOICED, SEG_UNVOICED, SEG_MIXED } seg_kind_t;

typedef struct {
    seg_kind_t kind;
    int        ms;       /* length */
    int        f0_from;  /* pitch at its start and end, Hz */
    int        f0_to;
    int        vowel;
    int        gain;     /* Q8 */
} seg_t;

static const seg_t s_script[] = {
    { SEG_SILENCE, 300, 0, 0, 0, 0 },
    { SEG_VOICED, 700, 110, 160, 0, 256 },
    { SEG_UNVOICED, 200, 0, 0, 0, 96 },
    { SEG_VOICED, 600, 180, 230, 1, 200 },
    { SEG_SILENCE, 200, 0, 0, 0, 0 },
    { SEG_MIXED, 400, 140, 120, 2, 220 },
    { SEG_VOICED, 500, 95, 85, 3, 320 },
    { SEG_UNVOICED, 300, 0, 0, 0, 160 },
    { SEG_VOICED, 800, 200, 120, 0, 120 },
    { SEG_SILENCE, 150, 0, 0, 0, 0 },
    { SEG_VOICED, 400, 260, 300, 1, 180 },
    { SEG_MIXED, 450, 160, 200, 3, 256 },
    { SEG_UNVOICED, 250, 0, 0, 0, 64 },
    { SEG_SILENCE, 0, 0, 0, 0, 0 } /* the rest */
};

static void make_signal(int16_t * out, int n)
{
    reso_t f1 = { 0 }, f2 = { 0 };
    uint32_t phase = 0; /* pitch phase, Q16 of a period */
    int i = 0;
    size_t s;

    for ( s = 0; s < sizeof(s_script) / sizeof(s_script[0]) && i < n; s++ )
    {
        const seg_t * g = &s_script[s];
        int len = g->ms ? g->ms * FS / 1000 : n - i;
        int k;

        if ( g->kind == SEG_VOICED || g->kind == SEG_MIXED )
        {
            const vowel_t * v = &s_vowels[g->vowel];
            f1.a1 = v->f1[0]; f1.a2 = v->f1[1]; f1.g = v->f1[2];
            f2.a1 = v->f2[0]; f2.a2 = v->f2[1]; f2.g = v->f2[2];
        }
        for ( k = 0; k < len && i < n; k++, i++ )
        {
            int32_t x = 0, y;
            /* a 10 ms attack and release on every segment */
            int32_t env = 256;
            int ramp = FS / 100;
            if ( k < ramp ) env = 256 * k / ramp;
            if ( len - k < ramp ) env = 256 * (len - k) / ramp;

            switch ( g->kind )
            {
            case SEG_SILENCE:
                y = noise() / 1024; /* a noise floor, about -60 dB */
                break;
            case SEG_UNVOICED:
                y = noise() / 2;
                y = y * g->gain / 256;
                break;
            case SEG_VOICED:
            case SEG_MIXED:
            {
                int f0 = g->f0_from + (g->f0_to - g->f0_from) * k / len;
                uint32_t step = (uint32_t)f0 * 65536u / FS;
                phase += step;
                if ( phase >= 65536u )
                {
                    phase -= 65536u;
                    x = 12000; /* a glottal pulse */
                }
                if ( g->kind == SEG_MIXED )
                {
                    x += noise() / 16;
                }
                y = reso_run(&f2, reso_run(&f1, x * 256)) / 4;
                y = y * g->gain / 256;
                break;
            }
            default:
                y = 0;
                break;
            }
            y = y * env / 256;
            if ( y > 32767 ) y = 32767;
            if ( y < -32768 ) y = -32768;
            out[i] = (int16_t)y;
        }
    }
    for ( ; i < n; i++ )
    {
        out[i] = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ file helpers */

static void * slurp(const char * path, size_t * len)
{
    FILE * f = fopen(path, "rb");
    void * buf;
    long sz;
    if ( !f ) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(sz ? (size_t)sz : 1);
    if ( !buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz ) { perror(path); exit(2); }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static void spill(const char * path, const void * buf, size_t len)
{
    FILE * f = fopen(path, "wb");
    if ( !f || fwrite(buf, 1, len, f) != len ) { perror(path); exit(2); }
    fclose(f);
}

static struct CODEC2 * open_mode(const char * arg)
{
    int mode = atoi(arg);
    struct CODEC2 * c;
    if ( mode < 0 || mode > 7 ) /* 3200 .. 700B */ { fprintf(stderr, "bad mode %s\n", arg); exit(2); }
    c = codec2_create(mode);
    if ( !c ) { fprintf(stderr, "codec2_create(%d) failed\n", mode); exit(2); }
    return c;
}

/* ------------------------------------------------------------------------------------------------ commands */

static int cmd_gen(const char * out)
{
    int16_t * s = malloc(sizeof(int16_t) * NSAMPLES);
    make_signal(s, NSAMPLES);
    spill(out, s, sizeof(int16_t) * NSAMPLES);
    free(s);
    return 0;
}

static int cmd_enc(const char * mode, const char * in, const char * out)
{
    struct CODEC2 * c = open_mode(mode);
    int nsam = codec2_samples_per_frame(c);
    int nbyte = (codec2_bits_per_frame(c) + 7) / 8;
    size_t len, frames, f;
    int16_t * speech = slurp(in, &len);
    unsigned char * bits;

    frames = len / sizeof(int16_t) / (size_t)nsam;
    bits = calloc(frames ? frames : 1, (size_t)nbyte);
    for ( f = 0; f < frames; f++ )
    {
        codec2_encode(c, bits + f * (size_t)nbyte, speech + f * (size_t)nsam);
    }
    spill(out, bits, frames * (size_t)nbyte);
    codec2_destroy(c);
    free(bits);
    free(speech);
    return 0;
}

static int cmd_dec(const char * mode, const char * in, const char * out)
{
    struct CODEC2 * c = open_mode(mode);
    int nsam = codec2_samples_per_frame(c);
    int nbyte = (codec2_bits_per_frame(c) + 7) / 8;
    size_t len, frames, f;
    unsigned char * bits = slurp(in, &len);
    int16_t * speech;

    codec2_set_lpc_post_filter(c, PF_ON, PF_BASS, PF_BETA, PF_GAMMA);
    frames = len / (size_t)nbyte;
    speech = calloc(frames ? frames * (size_t)nsam : 1, sizeof(int16_t));
    for ( f = 0; f < frames; f++ )
    {
        codec2_decode(c, speech + f * (size_t)nsam, bits + f * (size_t)nbyte);
    }
    spill(out, speech, frames * (size_t)nsam * sizeof(int16_t));
    codec2_destroy(c);
    free(bits);
    free(speech);
    return 0;
}

/* Prints "<differing samples> <total> <max abs error> <SNR dB of b against a> <rms of a>". */
static int cmd_cmp(const char * a, const char * b)
{
    size_t la, lb, n, i, ndiff = 0;
    int16_t * x = slurp(a, &la);
    int16_t * y = slurp(b, &lb);
    double sig = 0.0, err = 0.0;
    int maxe = 0;

    n = (la < lb ? la : lb) / sizeof(int16_t);
    for ( i = 0; i < n; i++ )
    {
        int e = (int)x[i] - (int)y[i];
        if ( e ) ndiff++;
        if ( abs(e) > maxe ) maxe = abs(e);
        sig += (double)x[i] * x[i];
        err += (double)e * e;
    }
    printf("%zu %zu %d %.2f %.1f%s\n", ndiff, n, maxe, err > 0.0 ? 10.0 * log10(sig / err) : INFINITY,
           n ? sqrt(sig / (double)n) : 0.0, la != lb ? " length-differs" : "");
    free(x);
    free(y);
    return 0;
}

int main(int argc, char ** argv)
{
    if ( argc == 3 && !strcmp(argv[1], "gen") ) return cmd_gen(argv[2]);
    if ( argc == 5 && !strcmp(argv[1], "enc") ) return cmd_enc(argv[2], argv[3], argv[4]);
    if ( argc == 5 && !strcmp(argv[1], "dec") ) return cmd_dec(argv[2], argv[3], argv[4]);
    if ( argc == 4 && !strcmp(argv[1], "cmp") ) return cmd_cmp(argv[2], argv[3]);
    fprintf(stderr,
            "usage: %s gen OUT.raw | enc MODE IN.raw OUT.bit | dec MODE IN.bit OUT.raw | cmp A.raw B.raw\n",
            argv[0]);
    return 2;
}
