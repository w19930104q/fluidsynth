/* Test MIDI 2.0 UMP (Universal MIDI Packet) support, in particular the
 * per-note pitch bend (MIDI 2.0 Per-Note Pitch Bend message and the Note-On
 * "Pitch 7.9" attribute) and its independence from the traditional
 * channel-wide pitch bend. */

#include "test.h"
#include "fluidsynth.h"
#include "fluid_synth.h"
#include "fluid_voice.h"

#include <math.h>

/* UMP message types */
#define UMP_MT_MIDI1_CV 0x2u
#define UMP_MT_MIDI2_CV 0x4u

/* MIDI 2.0 Note-On attribute types */
#define UMP_ATTR_PITCH_7_9 0x03u

/* MIDI 2.0 Per-Note Pitch Bend status nibble */
#define UMP_STATUS_PER_NOTE_PITCH_BEND 0x60u

#define UMP_PITCH_BEND_CENTER 0x80000000u

static unsigned int make_ump_word0(unsigned int mt, unsigned int status,
                                   unsigned int chan, unsigned int index,
                                   unsigned int attr_or_data)
{
    return (mt << 28) | ((status | chan) << 16) | (index << 8) | attr_or_data;
}

/* converts a pitch in semitones to a MIDI 2.0 "Pitch 7.9" attribute value */
static unsigned int pitch_to_7_9(double semitones)
{
    return (unsigned int)(semitones * 512.0 + 0.5);
}

/* converts a per-note pitch bend in cents to a 32 bit MIDI 2.0 Per-Note
 * Pitch Bend value (fixed range of +/- 96 semitones) */
static unsigned int cents_to_per_note_pitch_bend(double cents)
{
    double v = (double)UMP_PITCH_BEND_CENTER
               + cents / 9600.0 * (double)UMP_PITCH_BEND_CENTER;
    return (unsigned int)(v + 0.5);
}

/* returns the first active voice playing key on chan, or NULL */
static fluid_voice_t *find_voice(fluid_synth_t *synth, int chan, int key)
{
    int i;

    for(i = 0; i < synth->polyphony; i++)
    {
        fluid_voice_t *voice = synth->voice[i];

        if(fluid_voice_is_on(voice) && voice->chan == chan && voice->key == key)
        {
            return voice;
        }
    }

    return NULL;
}

static void render_some(fluid_synth_t *synth)
{
    float left[128], right[128];
    TEST_SUCCESS(fluid_synth_write_float(synth, 64, left, 0, 1, right, 0, 1));
}

int main(void)
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_voice_t *voice61, *voice62, *voice64, *voice65, *voice67;
    fluid_real_t pitch_before, pitch_after;
    int chan_pitch_bend;

    TEST_DISABLE_STDOUT_BUFFERING();

    settings = new_fluid_settings();
    fluid_settings_setint(settings, "synth.dynamic-sample-loading", 0);
    synth = new_fluid_synth(settings);
    TEST_SUCCESS(fluid_synth_sfload(synth, TEST_SOUNDFONT, 1));

    /* --- Test 1: a legacy MIDI 1.0 note-on has no per-note pitch bend --- */
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 60, 100));
    TEST_ASSERT(find_voice(synth, 0, 60) != NULL);
    TEST_ASSERT(find_voice(synth, 0, 60)->note_pitch_bend == 0.0f);
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 60));

    /* --- Test 2: MIDI 2.0 note-on with "Pitch 7.9" attribute bends only
     * this note. Play key 61 with an absolute pitch of 61.5 semitones,
     * i.e. a per-note pitch bend of +50 cents. --- */
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, 0x90, 0, 61, UMP_ATTR_PITCH_7_9),
                                        ((100u << 9) << 16) | pitch_to_7_9(61.5)));
    voice61 = find_voice(synth, 0, 61);
    TEST_ASSERT(voice61 != NULL);
    TEST_ASSERT(fabs(voice61->note_pitch_bend - 50.0) < 0.01);

    /* a plain note-on on key 62 must stay unbent; the rendered pitch
     * difference between key 62 and bent key 61 must be 100 - 50 = 50 cents */
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 62, 100));
    voice62 = find_voice(synth, 0, 62);
    TEST_ASSERT(voice62 != NULL);
    TEST_ASSERT(voice62->note_pitch_bend == 0.0f);
    TEST_ASSERT(fabs((voice62->pitch - voice61->pitch) - 50.0) < 0.5);

    /* the channel pitch bend must be untouched (still centered) */
    TEST_SUCCESS(fluid_synth_get_pitch_bend(synth, 0, &chan_pitch_bend));
    TEST_ASSERT(chan_pitch_bend == 8192);

    /* --- Test 3: MIDI 2.0 Per-Note Pitch Bend message updates the active
     * note and stacks with the channel-wide pitch bend --- */
    /* bend key 61 down by 48 semitones (exactly representable) */
    pitch_before = voice61->pitch;
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, UMP_STATUS_PER_NOTE_PITCH_BEND, 0, 61, 0),
                                        cents_to_per_note_pitch_bend(-4800.0)));
    TEST_ASSERT(fabs(voice61->note_pitch_bend - -4800.0) < 0.01);
    pitch_after = voice61->pitch;
    /* the voice pitch must have moved by the delta of the per-note bend */
    TEST_ASSERT(fabs((pitch_after - pitch_before) - (-4800.0 - 50.0)) < 0.5);
    /* key 62 on the same channel is not affected */
    TEST_ASSERT(voice62->note_pitch_bend == 0.0f);

    /* now apply a channel-wide pitch bend (MIDI 1.0): it must shift both
     * voices of the channel by the same amount, while the per-note bend
     * stays as is */
    {
        fluid_real_t pitch61_before = voice61->pitch;
        fluid_real_t pitch62_before = voice62->pitch;
        fluid_real_t delta61, delta62;

        TEST_SUCCESS(fluid_synth_pitch_bend(synth, 0, 16383));
        TEST_ASSERT(fabs(voice61->note_pitch_bend - -4800.0) < 0.01);
        TEST_ASSERT(voice62->note_pitch_bend == 0.0f);

        delta61 = voice61->pitch - pitch61_before;
        delta62 = voice62->pitch - pitch62_before;
        /* the bend must actually do something (approx. +200 cents with the
         * default sensitivity of 2 semitones) ... */
        TEST_ASSERT(delta61 > 100.0);
        /* ... and it must be channel-wide, i.e. identical for both voices */
        TEST_ASSERT(fabs(delta61 - delta62) < 0.01);

        TEST_SUCCESS(fluid_synth_get_pitch_bend(synth, 0, &chan_pitch_bend));
        TEST_ASSERT(chan_pitch_bend == 16383);
        TEST_SUCCESS(fluid_synth_pitch_bend(synth, 0, 8192));
    }

    /* --- Test 4: two simultaneous notes bend independently --- */
    /* key 64: no bend (Pitch 7.9 attribute equal to the note number),
     * key 65: +100 cents via Pitch 7.9 attribute */
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, 0x90, 0, 64, UMP_ATTR_PITCH_7_9),
                                        ((100u << 9) << 16) | pitch_to_7_9(64.0)));
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, 0x90, 0, 65, UMP_ATTR_PITCH_7_9),
                                        ((100u << 9) << 16) | pitch_to_7_9(66.0)));
    voice64 = find_voice(synth, 0, 64);
    voice65 = find_voice(synth, 0, 65);
    TEST_ASSERT(voice64 != NULL && voice65 != NULL);
    TEST_ASSERT(fabs(voice64->note_pitch_bend) < 0.01);
    TEST_ASSERT(fabs(voice65->note_pitch_bend - 100.0) < 0.01);

    /* bend only key 64 up by 24 semitones */
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, UMP_STATUS_PER_NOTE_PITCH_BEND, 0, 64, 0),
                                        cents_to_per_note_pitch_bend(2400.0)));
    TEST_ASSERT(fabs(voice64->note_pitch_bend - 2400.0) < 0.01);
    TEST_ASSERT(fabs(voice65->note_pitch_bend - 100.0) < 0.01);

    /* reset key 64 to zero bend via the public API */
    TEST_SUCCESS(fluid_synth_note_pitch_bend(synth, 0, 64, 0.0f));
    TEST_ASSERT(voice64->note_pitch_bend == 0.0f);
    TEST_ASSERT(fabs(voice65->note_pitch_bend - 100.0) < 0.01);

    /* per-note pitch bend on a note that is not playing must not fail */
    TEST_SUCCESS(fluid_synth_note_pitch_bend(synth, 0, 100, 300.0f));

    /* make sure the bent voices actually render */
    render_some(synth);

    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 61));
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 62));
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, 0x80, 0, 64, 0),
                                        0));
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI2_CV, 0x80, 0, 65, 0),
                                        0));

    /* --- Test 5: MIDI 1.0 Channel Voice messages carried in UMP (message
     * type 0x2) behave exactly like legacy MIDI 1.0 input --- */
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI1_CV, 0x90, 0, 67, 100),
                                        0));
    voice67 = find_voice(synth, 0, 67);
    TEST_ASSERT(voice67 != NULL);
    TEST_ASSERT(voice67->note_pitch_bend == 0.0f);

    /* UMP MIDI 1.0 pitch bend is channel-wide: 0x7F7F = 16383 */
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI1_CV, 0xE0, 0, 0x7F, 0x7F),
                                        0));
    TEST_SUCCESS(fluid_synth_get_pitch_bend(synth, 0, &chan_pitch_bend));
    TEST_ASSERT(chan_pitch_bend == 16383);
    TEST_ASSERT(voice67->note_pitch_bend == 0.0f);
    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI1_CV, 0xE0, 0, 0x00, 0x40),
                                        0));
    TEST_SUCCESS(fluid_synth_get_pitch_bend(synth, 0, &chan_pitch_bend));
    TEST_ASSERT(chan_pitch_bend == 8192);

    TEST_SUCCESS(fluid_synth_handle_ump(synth,
                                        make_ump_word0(UMP_MT_MIDI1_CV, 0x80, 0, 67, 0),
                                        0));
    render_some(synth);

    /* --- Test 6: a reused voice must not inherit a stale per-note bend --- */
    TEST_SUCCESS(fluid_synth_noteon(synth, 0, 61, 100));
    voice61 = find_voice(synth, 0, 61);
    TEST_ASSERT(voice61 != NULL);
    TEST_ASSERT(voice61->note_pitch_bend == 0.0f);
    TEST_SUCCESS(fluid_synth_noteoff(synth, 0, 61));

    /* --- Test 7: invalid input is rejected --- */
    TEST_ASSERT(fluid_synth_handle_ump(NULL, 0, 0) == FLUID_FAILED);
    /* unsupported message type (e.g. system messages, mt 0x1) */
    TEST_ASSERT(fluid_synth_handle_ump(synth, 0x10000000u, 0) == FLUID_FAILED);
    /* per-note pitch bend with invalid key */
    TEST_ASSERT(fluid_synth_note_pitch_bend(synth, 0, 128, 0.0f) == FLUID_FAILED);

    delete_fluid_synth(synth);
    delete_fluid_settings(settings);

    return EXIT_SUCCESS;
}
