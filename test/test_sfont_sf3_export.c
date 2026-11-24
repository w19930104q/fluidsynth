#include "test.h"
#include "fluidsynth.h"
#include "sfloader/fluid_sfont.h"
#include "sfloader/fluid_defsfont.h"
#include "utils/fluid_sys.h"
#include "utils/fluid_list.h"

#include <math.h>

/* Load a SoundFont, modify preset/instrument parameters at runtime, export
 * the SoundFont as SF3, load it back and verify that the modifications (and
 * the rest of the SoundFont structure) survived the round-trip. */
static void test_roundtrip(fluid_settings_t *settings, const char *source, int dynamic, const char *sf3_file)
{
    fluid_synth_t *synth, *synth2;
    fluid_sfont_t *sfont, *sfont2;
    fluid_defsfont_t *defsfont, *defsfont2;
    fluid_defpreset_t *preset, *preset2;
    fluid_preset_zone_t *pzone, *pzone2;
    fluid_inst_zone_t *izone, *izone2;
    fluid_sample_t *sample, *sample2;
    fluid_mod_t *mod;
    fluid_list_t *l;
    unsigned int bank, num;
    int id, count, count2;
    unsigned int frames, frames2;

    fluid_settings_setint(settings, "synth.dynamic-sample-loading", dynamic);

    /* load the source SoundFont */
    synth = new_fluid_synth(settings);
    id = fluid_synth_sfload(synth, source, 0);
    TEST_ASSERT(id != FLUID_FAILED);

    sfont = fluid_synth_get_sfont_by_id(synth, id);
    TEST_ASSERT(sfont != NULL);
    defsfont = fluid_sfont_get_data(sfont);
    TEST_ASSERT(defsfont != NULL);
    TEST_ASSERT(defsfont->preset != NULL);
    TEST_ASSERT(defsfont->sample != NULL);

    /* modify generator values at runtime: pan and attenuation on the first
     * preset zone, coarse tune on its first instrument zone */
    preset = fluid_preset_get_data(fluid_list_get(defsfont->preset));
    TEST_ASSERT(preset != NULL);
    bank = preset->bank;
    num = preset->num;

    pzone = preset->zone;
    TEST_ASSERT(pzone != NULL);
    pzone->gen[GEN_PAN].val = 123;
    pzone->gen[GEN_PAN].flags = GEN_SET;
    pzone->gen[GEN_ATTENUATION].val = 4; /* scaled domain, exports as 10 */
    pzone->gen[GEN_ATTENUATION].flags = GEN_SET;

    izone = pzone->inst != NULL ? pzone->inst->zone : NULL;
    TEST_ASSERT(izone != NULL);
    izone->gen[GEN_COARSETUNE].val = 5;
    izone->gen[GEN_COARSETUNE].flags = GEN_SET;

    /* add a custom modulator to the preset zone */
    mod = new_fluid_mod();
    TEST_ASSERT(mod != NULL);
    fluid_mod_set_source1(mod, FLUID_MOD_VELOCITY,
                          FLUID_MOD_GC | FLUID_MOD_CONCAVE | FLUID_MOD_NEGATIVE | FLUID_MOD_UNIPOLAR);
    fluid_mod_set_source2(mod, FLUID_MOD_NONE, FLUID_MOD_GC);
    fluid_mod_set_dest(mod, GEN_FILTERFC);
    fluid_mod_set_amount(mod, 750);
    mod->next = pzone->mod;
    pzone->mod = mod;

    sample = izone->sample;
    TEST_ASSERT(sample != NULL);

    /* export the modified SoundFont as SF3 (this also makes sure the sample
     * data of the original is loaded, even with dynamic sample loading) */
    TEST_SUCCESS(fluid_sfont_export_sf3(sfont, sf3_file));
    TEST_ASSERT(fluid_is_soundfont(sf3_file));

    /* capture the sample properties after the export: at this point the
     * sample data is loaded and start/loop points are in the same "loaded"
     * state as they will be after the reload */
    frames = sample->end - sample->start + 1;

    /* count presets and samples of the original for later comparison */
    for(l = defsfont->preset, count = 0; l != NULL; l = fluid_list_next(l))
    {
        count++;
    }

    for(l = defsfont->sample, count2 = 0; l != NULL; l = fluid_list_next(l))
    {
        count2++;
    }

    /* load the exported SF3 into a new synth */
    synth2 = new_fluid_synth(settings);
    id = fluid_synth_sfload(synth2, sf3_file, 0);
    TEST_ASSERT(id != FLUID_FAILED);

    sfont2 = fluid_synth_get_sfont_by_id(synth2, id);
    TEST_ASSERT(sfont2 != NULL);
    defsfont2 = fluid_sfont_get_data(sfont2);
    TEST_ASSERT(defsfont2 != NULL);

    /* same number of presets and samples */
    {
        int c;
        fluid_list_t *l2;

        for(l2 = defsfont2->preset, c = 0; l2 != NULL; l2 = fluid_list_next(l2))
        {
            c++;
        }

        TEST_ASSERT(c == count);

        for(l2 = defsfont2->sample, c = 0; l2 != NULL; l2 = fluid_list_next(l2))
        {
            c++;
        }

        TEST_ASSERT(c == count2);
    }

    /* find the modified preset by bank/number and verify the generators */
    preset2 = NULL;

    for(l = defsfont2->preset; l != NULL; l = fluid_list_next(l))
    {
        fluid_defpreset_t *p = fluid_preset_get_data(fluid_list_get(l));

        if(p != NULL && p->bank == bank && p->num == num)
        {
            preset2 = p;
            break;
        }
    }

    TEST_ASSERT(preset2 != NULL);

    /* select the preset to make sure its samples are loaded (they are byte
     * offsets into the compressed stream until then when dynamic sample
     * loading is enabled) */
    TEST_SUCCESS(fluid_synth_program_select(synth2, 0, id, bank, num));

    pzone2 = preset2->zone;
    TEST_ASSERT(pzone2 != NULL);
    TEST_ASSERT(pzone2->gen[GEN_PAN].flags == GEN_SET);
    TEST_ASSERT(pzone2->gen[GEN_PAN].val == 123);
    TEST_ASSERT(pzone2->gen[GEN_ATTENUATION].flags == GEN_SET);
    /* the attenuation is scaled by 0.4f on import, allow a rounding error */
    TEST_ASSERT(fabs(pzone2->gen[GEN_ATTENUATION].val - 4) < 0.001);

    /* the custom modulator must have survived the round-trip */
    for(mod = pzone2->mod; mod != NULL; mod = mod->next)
    {
        if(mod->dest == GEN_FILTERFC)
        {
            break;
        }
    }

    TEST_ASSERT(mod != NULL);
    TEST_ASSERT(mod->src1 == FLUID_MOD_VELOCITY);
    TEST_ASSERT(mod->flags1 & FLUID_MOD_CONCAVE);
    TEST_ASSERT(mod->flags1 & FLUID_MOD_NEGATIVE);
    TEST_ASSERT(!(mod->flags1 & FLUID_MOD_BIPOLAR));
    TEST_ASSERT(mod->amount == 750);

    izone2 = pzone2->inst != NULL ? pzone2->inst->zone : NULL;
    TEST_ASSERT(izone2 != NULL);
    TEST_ASSERT(izone2->gen[GEN_COARSETUNE].flags == GEN_SET);
    TEST_ASSERT(izone2->gen[GEN_COARSETUNE].val == 5);

    /* the sample of that zone must have kept its properties (the Ogg Vorbis
     * stream must have been decompressed back to the same frame count) */
    sample2 = izone2->sample;
    TEST_ASSERT(sample2 != NULL);
    TEST_ASSERT(FLUID_STRCMP(sample2->name, sample->name) == 0);
    TEST_ASSERT(sample2->sampletype & FLUID_SAMPLETYPE_OGG_VORBIS);
    TEST_ASSERT(sample2->samplerate == sample->samplerate);
    TEST_ASSERT(sample2->origpitch == sample->origpitch);
    TEST_ASSERT(sample2->pitchadj == sample->pitchadj);

    frames2 = sample2->end - sample2->start + 1;
    TEST_ASSERT(frames2 == frames);

    /* loop points are stored relative to the sample start in both cases */
    TEST_ASSERT(sample2->loopstart - sample2->start == sample->loopstart - sample->start);
    TEST_ASSERT(sample2->loopend - sample2->start == sample->loopend - sample->start);

    delete_fluid_synth(synth2);
    delete_fluid_synth(synth);

    remove(sf3_file);
}

int main(void)
{
    fluid_settings_t *settings = new_fluid_settings();

    /* SF2 and SF3 sources, each with and without dynamic sample loading */
    test_roundtrip(settings, TEST_SOUNDFONT, 0, "test_sfont_sf3_export_1.sf3");
    test_roundtrip(settings, TEST_SOUNDFONT, 1, "test_sfont_sf3_export_2.sf3");
    test_roundtrip(settings, TEST_SOUNDFONT_SF3, 0, "test_sfont_sf3_export_3.sf3");
    test_roundtrip(settings, TEST_SOUNDFONT_SF3, 1, "test_sfont_sf3_export_4.sf3");

    delete_fluid_settings(settings);

    return EXIT_SUCCESS;
}
