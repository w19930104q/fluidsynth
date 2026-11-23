
#include "test.h"
#include "fluidsynth.h"
#include "fluidsynth_priv.h"
#include "fluid_synth.h"
#include "fluid_voice.h"
#include "fluid_rvoice.h"

/*
 * Per-channel effect send (MIDI CC#91 reverb send, CC#93 chorus send).
 *
 * GM/GS/XG allow the effect send amount to be set per MIDI channel. The
 * synth must apply each voice's own send level when mixing its signal into
 * the reverb/chorus send buffers, and the DSP effect buffers must correctly
 * accumulate contributions from voices with different send levels.
 *
 * This file verifies, end to end:
 *   1. CC#91/#93 reach the per-voice send level and rvoice buffer amplitude
 *      (both at note-on and via live modulation of a playing voice).
 *   2. Send levels only affect the wet path: a dry render is identical for
 *      any send amount.
 *   3. Rendered reverb/chorus wet energy follows the send level.
 *   4. Wet contributions of channels with different send levels add
 *      correctly (linear superposition).
 *   5. The same holds when the events arrive through a MIDI file player.
 */

#define TEST_SF2 "sf2/VintageDreamsWaves-v2.sf2"
#define SR 44100.0
#define NSAMP ((int)(SR * 2.5))
#define BLK 1024

static fluid_synth_t *make_synth(fluid_settings_t **out_settings, int reverb,
                                 int chorus)
{
    fluid_settings_t *settings = new_fluid_settings();
    fluid_synth_t *synth;

    TEST_ASSERT(settings != NULL);
    TEST_SUCCESS(fluid_settings_setnum(settings, "synth.sample-rate", SR));
    TEST_SUCCESS(fluid_settings_setint(settings, "synth.polyphony", 32));
    synth = new_fluid_synth(settings);
    TEST_ASSERT(synth != NULL);
    TEST_ASSERT(fluid_synth_sfload(synth, TEST_SF2, 1) >= 0);
    TEST_SUCCESS(fluid_synth_reverb_on(synth, -1, reverb));
    TEST_SUCCESS(fluid_synth_chorus_on(synth, -1, chorus));
    *out_settings = settings;
    return synth;
}

static void render_window(fluid_synth_t *synth, float *out)
{
    int done = 0;
    memset(out, 0, sizeof(float) * NSAMP * 2);
    while(done + BLK <= NSAMP)
    {
        TEST_SUCCESS(fluid_synth_write_float(synth, BLK,
                        out + done * 2, 0, 2,
                        out + done * 2 + 1, 1, 2));
        done += BLK;
    }
}

static fluid_voice_t *first_voice_on_channel(fluid_synth_t *synth, int chan)
{
    int i;
    for(i = 0; i < synth->polyphony; i++)
    {
        fluid_voice_t *v = synth->voice[i];
        if(fluid_voice_get_channel(v) == chan && fluid_voice_is_playing(v))
            return v;
    }
    return NULL;
}

/* 1. Control plane: CC#91/#93 -> voice send level -> rvoice buffer amp. */
static void test_control_plane(void)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth = make_synth(&settings, 1, 1);
    fluid_voice_t *v0, *v1;

    /* different send levels set before the notes */
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH1, 127));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH3, 127));

    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    TEST_SUCCESS(fluid_synth_noteon(synth, 1, 60, 100));
    TEST_SUCCESS(fluid_synth_process(synth, 4096, 0, NULL, 0, NULL));

    v0 = first_voice_on_channel(synth, 0);
    v1 = first_voice_on_channel(synth, 1);
    TEST_ASSERT(v0 != NULL && v1 != NULL);

    /* buffer 2 = reverb send, buffer 3 = chorus send */
    TEST_ASSERT(v0->reverb_send < v1->reverb_send);
    TEST_ASSERT(v0->chorus_send < v1->chorus_send);
    TEST_ASSERT(v0->rvoice->buffers.bufs[2].target_amp
                < v1->rvoice->buffers.bufs[2].target_amp);
    TEST_ASSERT(v0->rvoice->buffers.bufs[3].target_amp
                < v1->rvoice->buffers.bufs[3].target_amp);

    /* live modulation of the already playing voice on channel 0 */
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 127));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 127));
    TEST_SUCCESS(fluid_synth_process(synth, 4096, 0, NULL, 0, NULL));
    v0 = first_voice_on_channel(synth, 0);
    TEST_ASSERT(v0 != NULL);
    TEST_ASSERT(v0->reverb_send > 0.2f);
    TEST_ASSERT(v0->chorus_send > 0.4f);

    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
}

/* 2. Dry isolation: sends must not change the dry (effect-off) signal. */
static void test_dry_isolation(void)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    float *a = FLUID_ARRAY(float, NSAMP * 2);
    float *b = FLUID_ARRAY(float, NSAMP * 2);
    int i, differing = 0;

    synth = make_synth(&settings, 0, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render_window(synth, a);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    synth = make_synth(&settings, 0, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 127));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 127));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render_window(synth, b);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    for(i = 0; i < NSAMP * 2; i++)
        if(a[i] != b[i])
            differing++;
    TEST_ASSERT(differing == 0);

    FLUID_FREE(a);
    FLUID_FREE(b);
}

/* render one note with given sends; returns RMS energy of the wet component
 * (difference against the same render with the effect unit disabled) */
static double wet_rms(int cc91, int cc93, int which /*0=reverb,1=chorus*/)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    float *wet = FLUID_ARRAY(float, NSAMP * 2);
    float *dry = FLUID_ARRAY(float, NSAMP * 2);
    int i;
    double acc = 0.0;

    /* wet render: only the unit under test is enabled */
    synth = make_synth(&settings, which == 0, which == 1);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, cc91));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, cc93));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render_window(synth, wet);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    /* dry reference */
    synth = make_synth(&settings, 0, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, cc91));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, cc93));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render_window(synth, dry);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    for(i = 0; i < NSAMP * 2; i++)
    {
        double d = (double)wet[i] - (double)dry[i];
        acc += d * d;
    }
    FLUID_FREE(wet);
    FLUID_FREE(dry);
    return sqrt(acc / (NSAMP * 2));
}

/* 3 & 4. Rendered wet energy follows the per-channel send level. */
static void test_wet_follows_send(void)
{
    double r0 = wet_rms(0,   0, 0);
    double r64 = wet_rms(64,  0, 0);
    double r127 = wet_rms(127, 0, 0);
    double c0 = wet_rms(0, 0,   1);
    double c64 = wet_rms(0, 64,  1);
    double c127 = wet_rms(0, 127, 1);

    TEST_ASSERT(r0 < r64 && r64 < r127);
    TEST_ASSERT(r127 > 2.0 * r0);
    TEST_ASSERT(c0 < c64 && c64 < c127);
    TEST_ASSERT(c127 > c0);
}

/* 4. Linear superposition across channels with different send levels.
 * The reverb is an LTI system, so render(A) + render(B) == render(A+B). */
static void test_superposition(void)
{
    float *A = FLUID_ARRAY(float, NSAMP * 2);   /* ch0 alone, max send */
    float *B = FLUID_ARRAY(float, NSAMP * 2);   /* ch1 alone, zero send */
    float *AB = FLUID_ARRAY(float, NSAMP * 2);  /* both together */
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    int i;
    double err = 0.0, norm = 0.0;

    /* A: channel 0 only */
    synth = make_synth(&settings, 1, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 127));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    render_window(synth, A);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    /* B: channel 1 only */
    synth = make_synth(&settings, 1, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH1, 0));
    TEST_SUCCESS(fluid_synth_noteon(synth, 1, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 1, 60));
    render_window(synth, B);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    /* AB: both channels simultaneously, different sends */
    synth = make_synth(&settings, 1, 0);
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH3, 0));
    TEST_SUCCESS(fluid_synth_cc(synth, 0, EFFECTS_DEPTH1, 127));
    TEST_SUCCESS(fluid_synth_cc(synth, 1, EFFECTS_DEPTH1, 0));
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 110));
    TEST_SUCCESS(fluid_synth_noteon(synth, 1, 60, 110));
    TEST_SUCCESS(fluid_synth_process(synth, 1024, 0, NULL, 0, NULL));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 1, 60));
    render_window(synth, AB);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    for(i = 0; i < NSAMP * 2; i++)
    {
        double d = (double)AB[i] - ((double)A[i] + (double)B[i]);
        err += d * d;
        norm += (double)AB[i] * AB[i];
    }
    /* only floating-point round-off is allowed */
    TEST_ASSERT(norm > 0.0);
    TEST_ASSERT(sqrt(err / norm) < 1e-5);

    FLUID_FREE(A);
    FLUID_FREE(B);
    FLUID_FREE(AB);
}

/* -- minimal SMF (format 0) construction with per-channel sends -- */

static void put_be32(unsigned char *p, unsigned int v)
{
    p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;  p[3] = v & 0xff;
}
static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (v >> 8) & 0xff; p[1] = v & 0xff;
}

/* append a channel event with a VLQ-encoded delta time */
static int append_evt(unsigned char *p, int dt, int status, int d1, int d2)
{
    unsigned char vlq[4];
    int vn = 0, n = 0, v = dt;
    vlq[vn++] = v & 0x7f;
    while((v >>= 7) > 0)
        vlq[vn++] = (v & 0x7f) | 0x80;
    while(vn > 0)
        p[n++] = vlq[--vn];
    p[n++] = (unsigned char)status;
    p[n++] = (unsigned char)d1;
    if(d2 >= 0)
        p[n++] = (unsigned char)d2;
    return n;
}

static int build_midi(unsigned char *buf, int cc91_a, int cc93_a,
                      int cc91_b, int cc93_b)
{
    unsigned char trk[256];
    int n = 0, trklen;

    /* all control changes / note-ons at tick 0 */
    n += append_evt(trk + n, 0, 0xb0, 91, cc91_a);
    n += append_evt(trk + n, 0, 0xb0, 93, cc93_a);
    n += append_evt(trk + n, 0, 0xb1, 91, cc91_b);
    n += append_evt(trk + n, 0, 0xb1, 93, cc93_b);
    n += append_evt(trk + n, 0, 0x90, 60, 110);
    n += append_evt(trk + n, 0, 0x91, 60, 110);
    /* note offs after 240 ticks (VLQ encoded) */
    n += append_evt(trk + n, 240, 0x80, 60, 0);
    n += append_evt(trk + n, 0, 0x81, 60, 0);
    /* end-of-track meta: delta=0, meta, type=0x2F, len=0 */
    trk[n++] = 0x00; trk[n++] = 0xff; trk[n++] = 0x2f; trk[n++] = 0x00;
    trklen = n;

    n = 0;
    memcpy(buf + n, "MThd", 4); n += 4;
    put_be32(buf + n, 6); n += 4;
    put_be16(buf + n, 0); n += 2;       /* format 0 */
    put_be16(buf + n, 1); n += 2;       /* one track */
    put_be16(buf + n, 480); n += 2;     /* division */
    memcpy(buf + n, "MTrk", 4); n += 4;
    put_be32(buf + n, trklen); n += 4;
    memcpy(buf + n, trk, trklen); n += trklen;
    return n;
}

static void render_midi_file(const unsigned char *midi, int len,
                             int reverb, int chorus, float *out)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_midi_router_t *router;
    fluid_player_t *player;

    synth = make_synth(&settings, reverb, chorus);
    router = new_fluid_midi_router(settings,
                                   fluid_synth_handle_midi_event, synth);
    TEST_ASSERT(router != NULL);
    player = new_fluid_player(synth);
    TEST_ASSERT(player != NULL);
    TEST_SUCCESS(fluid_player_add_mem(player, midi, len));
    TEST_SUCCESS(fluid_player_set_playback_callback(player,
                        fluid_midi_router_handle_midi_event, router));
    TEST_SUCCESS(fluid_player_play(player));
    render_window(synth, out);

    delete_fluid_player(player);
    delete_fluid_midi_router(router);
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
}

/* 5. MIDI file player path: differing per-channel sends -> differing wet. */
static void test_midi_file_path(void)
{
    unsigned char hi[300], lo[300], diff[300];
    int n_hi = build_midi(hi, 127, 127, 127, 127);
    int n_lo = build_midi(lo, 0, 0, 0, 0);
    int n_diff = build_midi(diff, 127, 127, 0, 0);
    float *wet = FLUID_ARRAY(float, NSAMP * 2);
    float *dry = FLUID_ARRAY(float, NSAMP * 2);
    double rev_lo, rev_diff, rev_hi, cho_lo, cho_diff, cho_hi;
    int i;
    double acc;

    /* reverb wet energy for the three MIDI files */
    #define REV_WET(buf, len, result)                                       \
        do {                                                                \
            render_midi_file(buf, len, 1, 0, wet);                          \
            render_midi_file(buf, len, 0, 0, dry);                          \
            acc = 0.0;                                                      \
            for(i = 0; i < NSAMP * 2; i++) {                                \
                double d = (double)wet[i] - (double)dry[i];                 \
                acc += d * d;                                               \
            }                                                               \
            result = sqrt(acc / (NSAMP * 2));                               \
        } while(0)

    REV_WET(lo, n_lo, rev_lo);
    REV_WET(diff, n_diff, rev_diff);
    REV_WET(hi, n_hi, rev_hi);
    TEST_ASSERT(rev_lo < rev_diff && rev_diff < rev_hi);

    #undef REV_WET
    #define CHO_WET(buf, len, result)                                       \
        do {                                                                \
            render_midi_file(buf, len, 0, 1, wet);                          \
            render_midi_file(buf, len, 0, 0, dry);                          \
            acc = 0.0;                                                      \
            for(i = 0; i < NSAMP * 2; i++) {                                \
                double d = (double)wet[i] - (double)dry[i];                 \
                acc += d * d;                                               \
            }                                                               \
            result = sqrt(acc / (NSAMP * 2));                               \
        } while(0)

    CHO_WET(lo, n_lo, cho_lo);
    CHO_WET(diff, n_diff, cho_diff);
    CHO_WET(hi, n_hi, cho_hi);
    TEST_ASSERT(cho_lo < cho_diff && cho_diff < cho_hi);

    FLUID_FREE(wet);
    FLUID_FREE(dry);
}

int main(void)
{
    test_control_plane();
    test_dry_isolation();
    test_wet_follows_send();
    test_superposition();
    test_midi_file_path();

    return EXIT_SUCCESS;
}
