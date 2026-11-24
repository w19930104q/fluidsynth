/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */

/*
 * SoundFont 3 (SF3) export.
 *
 * This is the reverse operation of the loader in fluid_sffile.c and
 * fluid_defsfont.c: it serializes the runtime structures (fluid_defsfont_t,
 * fluid_defpreset_t, fluid_inst_t, zones, generators, modulators and
 * samples) back into the SoundFont RIFF container. Samples are always
 * compressed as Ogg Vorbis streams stored back to back in the smpl chunk,
 * as mandated by the SF3 format (the shdr sample start/end offsets are byte
 * offsets into that chunk, loop points are relative to the decompressed
 * sample start).
 */

#include "fluid_sfwrite.h"
#include "fluid_sffile.h"
#include "fluid_sys.h"

#include <math.h>
#include <string.h>

#if LIBSNDFILE_SUPPORT

#include <sndfile.h>

/* Same factor the loader applies to GEN_ATTENUATION on import (see fluid_defsfont.c),
 * the exporter applies the inverse transformation. */
#define SFW_EMU_ATTENUATION_FACTOR (0.4)

/* Record sizes of the SoundFont hydra (pdta) chunks */
#define SFW_PHDR_SIZE 38
#define SFW_IHDR_SIZE 22
#define SFW_SHDR_SIZE 46
#define SFW_BAG_SIZE 4
#define SFW_MOD_SIZE 10
#define SFW_GEN_SIZE 4

/*--------------------------------------------------------------------------
 * RIFF writer (little endian, chunk sizes patched back on chunk close)
 *--------------------------------------------------------------------------*/
typedef struct
{
    FILE *fp;
    long chunk_stack[16]; /* file offsets of the unpatched chunk size fields */
    int level;
    int err;
} sfw_writer_t;

static void sfw_write(sfw_writer_t *w, const void *data, size_t len)
{
    if(w->err)
    {
        return;
    }

    if(len > 0 && fwrite(data, 1, len, w->fp) != len)
    {
        w->err = TRUE;
    }
}

static void sfw_u8(sfw_writer_t *w, unsigned int v)
{
    unsigned char b = (unsigned char)(v & 0xFF);
    sfw_write(w, &b, 1);
}

static void sfw_u16(sfw_writer_t *w, unsigned int v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    sfw_write(w, b, 2);
}

static void sfw_u32(sfw_writer_t *w, unsigned int v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
    sfw_write(w, b, 4);
}

static void sfw_zeros(sfw_writer_t *w, int count)
{
    static const unsigned char zeros[16] = { 0 };

    while(count > 0)
    {
        int n = count < (int)sizeof(zeros) ? count : (int)sizeof(zeros);
        sfw_write(w, zeros, (size_t)n);
        count -= n;
    }
}

/* Write a 20 byte, zero padded SoundFont name field */
static void sfw_name(sfw_writer_t *w, const char *name)
{
    char buf[20];
    size_t len;

    memset(buf, 0, sizeof(buf));

    if(name != NULL)
    {
        len = strlen(name);

        if(len > sizeof(buf))
        {
            len = sizeof(buf);
        }

        memcpy(buf, name, len);
    }

    sfw_write(w, buf, sizeof(buf));
}

static void sfw_begin_chunk(sfw_writer_t *w, const char id[4])
{
    long pos;

    if(w->err)
    {
        return;
    }

    sfw_write(w, id, 4);
    pos = ftell(w->fp);

    if(pos < 0 || w->level >= (int)(sizeof(w->chunk_stack) / sizeof(w->chunk_stack[0])))
    {
        w->err = TRUE;
        return;
    }

    w->chunk_stack[w->level++] = pos;
    sfw_u32(w, 0); /* size placeholder, patched by sfw_end_chunk() */
}

static void sfw_end_chunk(sfw_writer_t *w)
{
    long size_pos, end;
    long size;

    if(w->err)
    {
        return;
    }

    end = ftell(w->fp);

    if(end < 0 || w->level <= 0)
    {
        w->err = TRUE;
        return;
    }

    size_pos = w->chunk_stack[--w->level];
    size = end - size_pos - 4;

    /* RIFF chunks are padded to even sizes, the pad is not part of the chunk size */
    if(size & 1)
    {
        sfw_u8(w, 0);
        end = ftell(w->fp);

        if(end < 0)
        {
            w->err = TRUE;
            return;
        }
    }

    if(fseek(w->fp, size_pos, SEEK_SET) != 0)
    {
        w->err = TRUE;
        return;
    }

    sfw_u32(w, (unsigned int)size);

    if(w->err || fseek(w->fp, end, SEEK_SET) != 0)
    {
        w->err = TRUE;
    }
}

static void sfw_begin_list(sfw_writer_t *w, const char list_id[4], const char type[4])
{
    sfw_begin_chunk(w, list_id);
    sfw_write(w, type, 4);
}

/*--------------------------------------------------------------------------
 * In-memory pdta (hydra) model
 *--------------------------------------------------------------------------*/
typedef struct
{
    char name[21];
    unsigned int prenum;
    unsigned int bank;
    unsigned int bagndx;
} sfw_phdr_t;

typedef struct
{
    char name[21];
    unsigned int bagndx;
} sfw_ihdr_t;

typedef struct
{
    unsigned int genndx;
    unsigned int modndx;
} sfw_bag_t;

typedef struct
{
    unsigned short id;
    short amount;
} sfw_genrec_t;

typedef struct
{
    char name[21];
    unsigned int start;     /* byte offset of the Ogg stream in the smpl chunk */
    unsigned int end;       /* byte offset of the end of the Ogg stream */
    unsigned int loopstart; /* frame offset, relative to the decompressed sample */
    unsigned int loopend;
    unsigned int samplerate;
    int origpitch;
    int pitchadj;
    unsigned short sampletype;
} sfw_shdr_t;

typedef struct
{
    sfw_phdr_t *phdr;
    unsigned int n_phdr, cap_phdr;
    sfw_bag_t *pbag;
    unsigned int n_pbag, cap_pbag;
    SFMod *pmod;
    unsigned int n_pmod, cap_pmod;
    sfw_genrec_t *pgen;
    unsigned int n_pgen, cap_pgen;
    sfw_ihdr_t *inst;
    unsigned int n_inst, cap_inst;
    sfw_bag_t *ibag;
    unsigned int n_ibag, cap_ibag;
    SFMod *imod;
    unsigned int n_imod, cap_imod;
    sfw_genrec_t *igen;
    unsigned int n_igen, cap_igen;
    sfw_shdr_t *shdr;
    unsigned int n_shdr, cap_shdr;
    unsigned char *smpl;
    size_t smpl_len, smpl_cap;
} sfw_model_t;

static int sfw_push(void **arr, unsigned int *count, unsigned int *cap, size_t elemsize)
{
    if(*count == *cap)
    {
        unsigned int newcap = *cap ? *cap * 2 : 16;
        void *p = FLUID_REALLOC(*arr, elemsize * newcap);

        if(p == NULL)
        {
            FLUID_LOG(FLUID_PANIC, "Out of memory");
            return FLUID_FAILED;
        }

        *arr = p;
        *cap = newcap;
    }

    (*count)++;
    return FLUID_OK;
}

/* Append rec to the array member "field" of model m, jumps to the "oom" label
 * of the calling function on allocation failure. */
#define SFW_PUSH(m, field, rec)                                                \
    do                                                                         \
    {                                                                          \
        if(sfw_push((void **)&(m)->field, &(m)->n_##field, &(m)->cap_##field,  \
                    sizeof(*(m)->field)) == FLUID_FAILED)                      \
        {                                                                      \
            goto oom;                                                          \
        }                                                                      \
        (m)->field[(m)->n_##field - 1] = (rec);                                \
    } while(0)

static void sfw_model_free(sfw_model_t *m)
{
    FLUID_FREE(m->phdr);
    FLUID_FREE(m->pbag);
    FLUID_FREE(m->pmod);
    FLUID_FREE(m->pgen);
    FLUID_FREE(m->inst);
    FLUID_FREE(m->ibag);
    FLUID_FREE(m->imod);
    FLUID_FREE(m->igen);
    FLUID_FREE(m->shdr);
    FLUID_FREE(m->smpl);
}

/*--------------------------------------------------------------------------
 * Conversion helpers
 *--------------------------------------------------------------------------*/

/* Generator IDs that must never appear in a SoundFont file (mirrors the
 * tables in fluid_sffile.c, cf. SoundFont spec 8.1.2 / 8.1.3) */
static const unsigned short sfw_invalid_inst_gen[] =
{
    GEN_UNUSED1, GEN_UNUSED2, GEN_UNUSED3, GEN_UNUSED4,
    GEN_RESERVED1, GEN_RESERVED2, GEN_RESERVED3, GEN_INSTRUMENT
};

static const unsigned short sfw_invalid_preset_gen[] =
{
    GEN_STARTADDROFS, GEN_ENDADDROFS, GEN_STARTLOOPADDROFS, GEN_ENDADDROFS,
    GEN_STARTADDRCOARSEOFS, GEN_ENDADDRCOARSEOFS, GEN_STARTLOOPADDRCOARSEOFS,
    GEN_KEYNUM, GEN_VELOCITY, GEN_ENDLOOPADDRCOARSEOFS, GEN_SAMPLEMODE,
    GEN_EXCLUSIVECLASS, GEN_OVERRIDEROOTKEY, GEN_SAMPLEID
};

static int sfw_gen_id_valid(int id, int is_preset_zone)
{
    size_t i;

    if(id < 0 || id > GEN_OVERRIDEROOTKEY)
    {
        return FALSE;
    }

    for(i = 0; i < FLUID_N_ELEMENTS(sfw_invalid_inst_gen); i++)
    {
        if((int)sfw_invalid_inst_gen[i] == id)
        {
            return FALSE;
        }
    }

    if(is_preset_zone)
    {
        for(i = 0; i < FLUID_N_ELEMENTS(sfw_invalid_preset_gen); i++)
        {
            if((int)sfw_invalid_preset_gen[i] == id)
            {
                return FALSE;
            }
        }
    }

    return TRUE;
}

/* Convert a runtime generator value back to its 16 bit SoundFont amount */
static short sfw_gen_amount(int id, const fluid_gen_t *gen)
{
    double v = gen->val;
    long l;

    if(id == GEN_ATTENUATION)
    {
        /* the loader scaled this generator by SFW_EMU_ATTENUATION_FACTOR */
        v /= SFW_EMU_ATTENUATION_FACTOR;
    }

    l = lrint(v);

    if(l > 32767)
    {
        l = 32767;
    }
    else if(l < -32768)
    {
        l = -32768;
    }

    return (short)l;
}

/* Convert one modulator source (index + flags) back to the 16 bit SFMod
 * source field (inverse of fluid_zone_mod_source_import_sfont) */
static unsigned short sfw_mod_source(unsigned char src, unsigned char flags)
{
    unsigned short s = src & 0x7F;

    if(flags & FLUID_MOD_CC)
    {
        s |= 1 << 7;
    }

    if(flags & FLUID_MOD_NEGATIVE)
    {
        s |= 1 << 8;
    }

    if(flags & FLUID_MOD_BIPOLAR)
    {
        s |= 1 << 9;
    }

    /* mapping type is stored in bits 2-3 of the flags (LINEAR, CONCAVE,
     * CONVEX, SWITCH) and in bits 10-15 of the SFMod source */
    s |= (unsigned short)(((flags >> 2) & 0x3) << 10);

    return s;
}

static int sfw_mod_to_sfmod(const fluid_mod_t *mod, SFMod *out)
{
    long amount;

    if((mod->flags1 & FLUID_MOD_CUSTOM) || (mod->flags2 & FLUID_MOD_CUSTOM) ||
            mod->mapping_func != NULL)
    {
        /* custom mapping functions cannot be represented in a SoundFont file */
        return FLUID_FAILED;
    }

    out->src = sfw_mod_source(mod->src1, mod->flags1);
    out->amtsrc = sfw_mod_source(mod->src2, mod->flags2);
    out->dest = mod->dest;
    out->trans = (mod->trans == FLUID_MOD_TRANSFORM_ABS) ? FLUID_MOD_TRANSFORM_ABS : 0;

    amount = lrint(mod->amount);

    if(amount > 32767)
    {
        amount = 32767;
    }
    else if(amount < -32768)
    {
        amount = -32768;
    }

    out->amount = (short)amount;
    return FLUID_OK;
}

static int sfw_sample_index(fluid_defsfont_t *defsfont, fluid_sample_t *sample)
{
    fluid_list_t *l;
    int i;

    for(l = defsfont->sample, i = 0; l != NULL; l = fluid_list_next(l), i++)
    {
        if(fluid_list_get(l) == sample)
        {
            return i;
        }
    }

    return -1;
}

static int sfw_inst_index(fluid_defsfont_t *defsfont, fluid_inst_t *inst)
{
    fluid_list_t *l;
    int i;

    for(l = defsfont->inst, i = 0; l != NULL; l = fluid_list_next(l), i++)
    {
        if(fluid_list_get(l) == inst)
        {
            return i;
        }
    }

    return -1;
}

/*--------------------------------------------------------------------------
 * Zone serialization
 *--------------------------------------------------------------------------*/

/* Emit the key or velocity range generator of a zone. The runtime default
 * for an unset range is 0-128 (see new_fluid_preset_zone()), anything else
 * is an effective range that must be written explicitly. This holds for
 * local and global zones alike: at import time missing ranges of local zones
 * are inherited from the global zone, so the runtime range is always the
 * effective one and writing it explicitly preserves the semantics exactly. */
static int sfw_emit_range(sfw_model_t *m, int is_preset_zone, int id, int lo, int hi)
{
    sfw_genrec_t rec;

    if(lo == 0 && hi == 128)
    {
        return FLUID_OK; /* unset range, nothing to write */
    }

    if(lo < 0)
    {
        lo = 0;
    }
    else if(lo > 127)
    {
        lo = 127;
    }

    if(hi < 0)
    {
        hi = 0;
    }
    else if(hi > 127)
    {
        hi = 127;
    }

    rec.id = (unsigned short)id;
    rec.amount = (short)(lo | (hi << 8));

    if(is_preset_zone)
    {
        SFW_PUSH(m, pgen, rec);
    }
    else
    {
        SFW_PUSH(m, igen, rec);
    }

    return FLUID_OK;

oom:
    return FLUID_FAILED;
}

/* Emit the generators and modulators of one zone into the preset or
 * instrument tables. The bag record must have been pushed by the caller. */
static int sfw_emit_zone_contents(sfw_model_t *m, fluid_gen_t *gen, fluid_zone_range_t *range,
                                  fluid_mod_t *mods, int is_global, int is_preset_zone,
                                  int ref_id, int ref_index)
{
    sfw_genrec_t rec;
    fluid_mod_t *mod;
    int i;

    /* Key and velocity ranges come first (SoundFont spec 8.1.2) */
    if(sfw_emit_range(m, is_preset_zone, GEN_KEYRANGE, range->keylo, range->keyhi) == FLUID_FAILED)
    {
        goto oom;
    }

    if(sfw_emit_range(m, is_preset_zone, GEN_VELRANGE, range->vello, range->velhi) == FLUID_FAILED)
    {
        goto oom;
    }

    for(i = 0; i <= GEN_OVERRIDEROOTKEY; i++)
    {
        if(i == GEN_KEYRANGE || i == GEN_VELRANGE || i == GEN_INSTRUMENT || i == GEN_SAMPLEID)
        {
            continue; /* handled separately */
        }

        if(gen[i].flags != GEN_SET)
        {
            continue;
        }

        if(!sfw_gen_id_valid(i, is_preset_zone))
        {
            continue;
        }

        rec.id = (unsigned short)i;
        rec.amount = sfw_gen_amount(i, &gen[i]);

        if(is_preset_zone)
        {
            SFW_PUSH(m, pgen, rec);
        }
        else
        {
            SFW_PUSH(m, igen, rec);
        }
    }

    /* the instrument/sampleID generator terminates the zone's generator list */
    if(!is_global && ref_index >= 0)
    {
        rec.id = (unsigned short)ref_id;
        rec.amount = (short)ref_index;

        if(is_preset_zone)
        {
            SFW_PUSH(m, pgen, rec);
        }
        else
        {
            SFW_PUSH(m, igen, rec);
        }
    }

    for(mod = mods; mod != NULL; mod = mod->next)
    {
        SFMod sm;

        if(sfw_mod_to_sfmod(mod, &sm) == FLUID_FAILED)
        {
            FLUID_LOG(FLUID_WARN, "SF3 export: skipping modulator with custom mapping function");
            continue;
        }

        if(is_preset_zone)
        {
            SFW_PUSH(m, pmod, sm);
        }
        else
        {
            SFW_PUSH(m, imod, sm);
        }
    }

    return FLUID_OK;

oom:
    return FLUID_FAILED;
}

static int sfw_preset_zone_count(fluid_defpreset_t *preset)
{
    int n = 0;
    fluid_preset_zone_t *zone;

    for(zone = preset->zone; zone != NULL; zone = zone->next)
    {
        n++;
    }

    return n;
}

static int sfw_inst_zone_count(fluid_inst_t *inst)
{
    int n = 0;
    fluid_inst_zone_t *zone;

    for(zone = inst->zone; zone != NULL; zone = zone->next)
    {
        n++;
    }

    return n;
}

static int sfw_build_presets(fluid_defsfont_t *defsfont, sfw_model_t *m)
{
    fluid_list_t *l;

    for(l = defsfont->preset; l != NULL; l = fluid_list_next(l))
    {
        fluid_defpreset_t *preset = fluid_preset_get_data(fluid_list_get(l));
        fluid_preset_zone_t *zone;
        sfw_phdr_t phdr;
        sfw_bag_t bag;
        int zi;

        if(preset == NULL)
        {
            continue;
        }

        memset(&phdr, 0, sizeof(phdr));
        FLUID_STRNCPY(phdr.name, preset->name, sizeof(phdr.name));
        phdr.prenum = preset->num;
        phdr.bank = preset->bank;
        phdr.bagndx = m->n_pbag;
        SFW_PUSH(m, phdr, phdr);

        if(preset->global_zone != NULL)
        {
            bag.genndx = m->n_pgen;
            bag.modndx = m->n_pmod;
            SFW_PUSH(m, pbag, bag);

            if(sfw_emit_zone_contents(m, preset->global_zone->gen, &preset->global_zone->range,
                                      preset->global_zone->mod, TRUE, TRUE, 0, -1) == FLUID_FAILED)
            {
                goto oom;
            }
        }

        /* The runtime zone list is in reverse file order (zones are prepended
         * on import, see fluid_defpreset_add_zone()). Emit the zones in
         * reverse, so that a reload of the exported file reproduces the same
         * runtime zone order. */
        for(zi = sfw_preset_zone_count(preset) - 1; zi >= 0; zi--)
        {
            int inst_idx = -1;
            int zj;

            for(zone = preset->zone, zj = 0; zj < zi; zj++)
            {
                zone = zone->next;
            }

            if(zone->inst != NULL)
            {
                inst_idx = sfw_inst_index(defsfont, zone->inst);

                if(inst_idx < 0)
                {
                    FLUID_LOG(FLUID_ERR, "SF3 export: instrument '%s' of preset '%s' not found",
                              zone->inst->name, preset->name);
                    return FLUID_FAILED;
                }
            }

            bag.genndx = m->n_pgen;
            bag.modndx = m->n_pmod;
            SFW_PUSH(m, pbag, bag);

            if(sfw_emit_zone_contents(m, zone->gen, &zone->range, zone->mod, FALSE, TRUE,
                                      GEN_INSTRUMENT, inst_idx) == FLUID_FAILED)
            {
                goto oom;
            }
        }
    }

    return FLUID_OK;

oom:
    return FLUID_FAILED;
}

static int sfw_build_instruments(fluid_defsfont_t *defsfont, sfw_model_t *m)
{
    fluid_list_t *l;

    for(l = defsfont->inst; l != NULL; l = fluid_list_next(l))
    {
        fluid_inst_t *inst = fluid_list_get(l);
        fluid_inst_zone_t *zone;
        sfw_ihdr_t ihdr;
        sfw_bag_t bag;
        int zi;

        memset(&ihdr, 0, sizeof(ihdr));
        FLUID_STRNCPY(ihdr.name, inst->name, sizeof(ihdr.name));
        ihdr.bagndx = m->n_ibag;
        SFW_PUSH(m, inst, ihdr);

        if(inst->global_zone != NULL)
        {
            bag.genndx = m->n_igen;
            bag.modndx = m->n_imod;
            SFW_PUSH(m, ibag, bag);

            if(sfw_emit_zone_contents(m, inst->global_zone->gen, &inst->global_zone->range,
                                      inst->global_zone->mod, TRUE, FALSE, 0, -1) == FLUID_FAILED)
            {
                goto oom;
            }
        }

        /* see sfw_build_presets() for why the zones are emitted in reverse */
        for(zi = sfw_inst_zone_count(inst) - 1; zi >= 0; zi--)
        {
            int sample_idx = -1;
            int zj;

            for(zone = inst->zone, zj = 0; zj < zi; zj++)
            {
                zone = zone->next;
            }

            if(zone->sample != NULL)
            {
                sample_idx = sfw_sample_index(defsfont, zone->sample);

                if(sample_idx < 0)
                {
                    FLUID_LOG(FLUID_ERR, "SF3 export: sample '%s' of instrument '%s' not found",
                              zone->sample->name, inst->name);
                    return FLUID_FAILED;
                }
            }

            bag.genndx = m->n_igen;
            bag.modndx = m->n_imod;
            SFW_PUSH(m, ibag, bag);

            if(sfw_emit_zone_contents(m, zone->gen, &zone->range, zone->mod, FALSE, FALSE,
                                      GEN_SAMPLEID, sample_idx) == FLUID_FAILED)
            {
                goto oom;
            }
        }
    }

    return FLUID_OK;

oom:
    return FLUID_FAILED;
}

/*--------------------------------------------------------------------------
 * Sample compression (Ogg Vorbis via libsndfile)
 *--------------------------------------------------------------------------*/

/* Growable in-memory file for libsndfile's virtual IO */
typedef struct
{
    unsigned char *data;
    sf_count_t size;
    sf_count_t cap;
    sf_count_t pos;
} sfw_memfile_t;

static sf_count_t sfw_vio_get_filelen(void *user_data)
{
    return ((sfw_memfile_t *)user_data)->size;
}

static sf_count_t sfw_vio_seek(sf_count_t offset, int whence, void *user_data)
{
    sfw_memfile_t *m = (sfw_memfile_t *)user_data;
    sf_count_t newpos;

    switch(whence)
    {
    case SEEK_SET:
        newpos = offset;
        break;

    case SEEK_CUR:
        newpos = m->pos + offset;
        break;

    case SEEK_END:
        newpos = m->size + offset;
        break;

    default:
        return -1;
    }

    if(newpos < 0)
    {
        return -1;
    }

    m->pos = newpos;
    return m->pos;
}

static sf_count_t sfw_vio_read(void *ptr, sf_count_t count, void *user_data)
{
    sfw_memfile_t *m = (sfw_memfile_t *)user_data;

    if(m->pos + count > m->size)
    {
        count = m->size - m->pos;
    }

    if(count > 0)
    {
        memcpy(ptr, m->data + m->pos, (size_t)count);
        m->pos += count;
    }

    return count;
}

static sf_count_t sfw_vio_write(const void *ptr, sf_count_t count, void *user_data)
{
    sfw_memfile_t *m = (sfw_memfile_t *)user_data;

    if(count <= 0)
    {
        return 0;
    }

    if(m->pos + count > m->cap)
    {
        sf_count_t newcap = m->cap ? m->cap : 4096;
        unsigned char *p;

        while(newcap < m->pos + count)
        {
            newcap *= 2;
        }

        p = FLUID_REALLOC(m->data, (size_t)newcap);

        if(p == NULL)
        {
            FLUID_LOG(FLUID_PANIC, "Out of memory");
            return 0;
        }

        m->data = p;
        m->cap = newcap;
    }

    if(m->pos > m->size)
    {
        /* zero-fill the gap left by a seek beyond the end of the stream */
        memset(m->data + m->size, 0, (size_t)(m->pos - m->size));
    }

    memcpy(m->data + m->pos, ptr, (size_t)count);
    m->pos += count;

    if(m->pos > m->size)
    {
        m->size = m->pos;
    }

    return count;
}

static sf_count_t sfw_vio_tell(void *user_data)
{
    return ((sfw_memfile_t *)user_data)->pos;
}

/* Encode mono 16 bit PCM data as an Ogg Vorbis stream */
static int sfw_encode_vorbis(const short *pcm, size_t frames, int samplerate, sfw_memfile_t *out)
{
    SF_VIRTUAL_IO vio =
    {
        sfw_vio_get_filelen,
        sfw_vio_seek,
        sfw_vio_read,
        sfw_vio_write,
        sfw_vio_tell
    };
    SF_INFO sfinfo;
    SNDFILE *sndfile;
    int ret = FLUID_FAILED;

    memset(&sfinfo, 0, sizeof(sfinfo));
    sfinfo.samplerate = samplerate > 0 ? samplerate : 44100;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;

    if(!sf_format_check(&sfinfo))
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: invalid Ogg Vorbis format parameters");
        return FLUID_FAILED;
    }

    sndfile = sf_open_virtual(&vio, SFM_WRITE, &sfinfo, out);

    if(sndfile == NULL)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: sf_open_virtual() failed: %s", sf_strerror(NULL));
        return FLUID_FAILED;
    }

    if(frames > 0 && sf_writef_short(sndfile, pcm, (sf_count_t)frames) != (sf_count_t)frames)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: failed to write Vorbis frames");
        goto out;
    }

    ret = FLUID_OK;

out:
    sf_close(sndfile);
    return ret;
}

static int sfw_build_samples(fluid_defsfont_t *defsfont, sfw_model_t *m)
{
    fluid_list_t *l;
    SFData *sffile = NULL;
    int ret = FLUID_FAILED;

    if(defsfont->sample24data != NULL)
    {
        FLUID_LOG(FLUID_INFO, "SF3 export: 24 bit sample data is truncated to 16 bit");
    }

    for(l = defsfont->sample; l != NULL; l = fluid_list_next(l))
    {
        fluid_sample_t *sample = fluid_list_get(l);
        sfw_shdr_t sh;
        sfw_memfile_t ogg;
        size_t frames = 0;
        unsigned int loopstart = 0, loopend = 0;

        memset(&ogg, 0, sizeof(ogg));

        if(sample->data == NULL && !fluid_sample_in_rom(sample))
        {
            /* sample data not in memory (dynamic sample loading): load it from
             * the source SoundFont file */
            if(sffile == NULL)
            {
                sffile = fluid_sffile_open(defsfont->filename, &defsfont->fcbs);

                if(sffile == NULL)
                {
                    FLUID_LOG(FLUID_ERR, "SF3 export: unable to reopen SoundFont file '%s' "
                              "to load sample data", defsfont->filename);
                    goto out;
                }
            }

            if(fluid_defsfont_load_sampledata(defsfont, sffile, sample) == FLUID_FAILED)
            {
                FLUID_LOG(FLUID_ERR, "SF3 export: unable to load sample '%s'", sample->name);
                goto out;
            }
        }

        if(sample->data != NULL && sample->end >= sample->start)
        {
            frames = (size_t)(sample->end - sample->start) + 1;
            loopstart = sample->loopstart >= sample->start ? sample->loopstart - sample->start : 0;
            loopend = sample->loopend >= sample->start ? sample->loopend - sample->start : 0;

            if(loopstart > frames)
            {
                loopstart = (unsigned int)frames;
            }

            if(loopend > frames)
            {
                loopend = (unsigned int)frames;
            }
        }

        if(sfw_encode_vorbis(sample->data ? sample->data + sample->start : NULL, frames,
                             (int)sample->samplerate, &ogg) == FLUID_FAILED)
        {
            FLUID_FREE(ogg.data);
            goto out;
        }

        memset(&sh, 0, sizeof(sh));
        FLUID_STRNCPY(sh.name, sample->name, sizeof(sh.name));
        sh.start = (unsigned int)m->smpl_len;
        sh.end = (unsigned int)(m->smpl_len + (size_t)ogg.size);
        sh.loopstart = loopstart;
        sh.loopend = loopend;
        sh.samplerate = sample->samplerate;
        sh.origpitch = sample->origpitch;
        sh.pitchadj = sample->pitchadj;
        sh.sampletype = (unsigned short)((sample->sampletype & ~FLUID_SAMPLETYPE_ROM) |
                                         FLUID_SAMPLETYPE_OGG_VORBIS);

        /* append the Ogg stream to the smpl chunk data */
        if(m->smpl_len + (size_t)ogg.size > m->smpl_cap)
        {
            size_t newcap = m->smpl_cap ? m->smpl_cap : 65536;
            unsigned char *p;

            while(newcap < m->smpl_len + (size_t)ogg.size)
            {
                newcap *= 2;
            }

            p = FLUID_REALLOC(m->smpl, newcap);

            if(p == NULL)
            {
                FLUID_LOG(FLUID_PANIC, "Out of memory");
                FLUID_FREE(ogg.data);
                goto out;
            }

            m->smpl = p;
            m->smpl_cap = newcap;
        }

        if(ogg.size > 0)
        {
            memcpy(m->smpl + m->smpl_len, ogg.data, (size_t)ogg.size);
            m->smpl_len += (size_t)ogg.size;
        }

        FLUID_FREE(ogg.data);
        SFW_PUSH(m, shdr, sh);
    }

    ret = FLUID_OK;

out:
    if(sffile != NULL)
    {
        fluid_sffile_close(sffile);
    }

    return ret;

oom:
    if(sffile != NULL)
    {
        fluid_sffile_close(sffile);
    }

    return FLUID_FAILED;
}

/*--------------------------------------------------------------------------
 * File serialization
 *--------------------------------------------------------------------------*/
static void sfw_write_mod_rec(sfw_writer_t *w, const SFMod *mod)
{
    sfw_u16(w, mod->src);
    sfw_u16(w, mod->dest);
    sfw_u16(w, (unsigned short)mod->amount);
    sfw_u16(w, mod->amtsrc);
    sfw_u16(w, mod->trans);
}

/* Write a zero terminated INFO string sub chunk. INFO sub chunk sizes must
 * be even (enforced by the loader), so the terminating zero is padded with
 * an additional zero inside the chunk if needed. */
static void sfw_write_info_string(sfw_writer_t *w, const char id[4], const char *str)
{
    size_t len = strlen(str) + 1;

    sfw_begin_chunk(w, id);
    sfw_write(w, str, len);

    if(len & 1)
    {
        sfw_u8(w, 0);
    }

    sfw_end_chunk(w);
}

static void sfw_write_info(sfw_writer_t *w, fluid_defsfont_t *defsfont)
{
    static const char isng[] = "EMU8000";
    const char *name, *basename;
    char isft[64];
    fluid_mod_t *mod;

    sfw_begin_list(w, "LIST", "INFO");

    /* SoundFont version 3.1 */
    sfw_begin_chunk(w, "ifil");
    sfw_u16(w, 3);
    sfw_u16(w, 1);
    sfw_end_chunk(w);

    sfw_begin_chunk(w, "isng");
    sfw_write(w, isng, sizeof(isng));
    sfw_end_chunk(w);

    /* use the base name of the source file as the SoundFont name */
    name = defsfont->filename ? defsfont->filename : "";
    basename = strrchr(name, '/');

    if(basename != NULL)
    {
        name = basename + 1;
    }

    basename = strrchr(name, '\\');

    if(basename != NULL)
    {
        name = basename + 1;
    }

    sfw_write_info_string(w, "INAM", name);

    FLUID_SNPRINTF(isft, sizeof(isft), "FluidSynth %s", FLUIDSYNTH_VERSION);
    sfw_write_info_string(w, "ISFT", isft);

    /* default modulators (https://github.com/spessasus/soundfont-proposals/blob/main/default_modulators.md) */
    if(defsfont->sfont != NULL && defsfont->sfont->default_mod_list != NULL)
    {
        sfw_begin_chunk(w, "DMOD");

        for(mod = defsfont->sfont->default_mod_list; mod != NULL; mod = mod->next)
        {
            SFMod sm;

            if(sfw_mod_to_sfmod(mod, &sm) == FLUID_FAILED)
            {
                FLUID_LOG(FLUID_WARN, "SF3 export: skipping default modulator with custom mapping function");
                continue;
            }

            sfw_write_mod_rec(w, &sm);
        }

        sfw_zeros(w, SFW_MOD_SIZE); /* terminal record */
        sfw_end_chunk(w);
    }

    sfw_end_chunk(w); /* LIST INFO */
}

static void sfw_write_pdta(sfw_writer_t *w, const sfw_model_t *m)
{
    unsigned int i;

    sfw_begin_list(w, "LIST", "pdta");

    /* preset headers */
    sfw_begin_chunk(w, "phdr");

    for(i = 0; i < m->n_phdr; i++)
    {
        sfw_name(w, m->phdr[i].name);
        sfw_u16(w, m->phdr[i].prenum);
        sfw_u16(w, m->phdr[i].bank);
        sfw_u16(w, m->phdr[i].bagndx);
        sfw_u32(w, 0); /* library */
        sfw_u32(w, 0); /* genre */
        sfw_u32(w, 0); /* morphology */
    }

    /* terminal record, carries the total bag count */
    sfw_zeros(w, 24);
    sfw_u16(w, m->n_pbag);
    sfw_zeros(w, 12);
    sfw_end_chunk(w);

    /* preset zone indices */
    sfw_begin_chunk(w, "pbag");

    for(i = 0; i < m->n_pbag; i++)
    {
        sfw_u16(w, m->pbag[i].genndx);
        sfw_u16(w, m->pbag[i].modndx);
    }

    sfw_u16(w, m->n_pgen); /* terminal record */
    sfw_u16(w, m->n_pmod);
    sfw_end_chunk(w);

    /* preset modulators */
    sfw_begin_chunk(w, "pmod");

    for(i = 0; i < m->n_pmod; i++)
    {
        sfw_write_mod_rec(w, &m->pmod[i]);
    }

    sfw_zeros(w, SFW_MOD_SIZE); /* terminal record */
    sfw_end_chunk(w);

    /* preset generators */
    sfw_begin_chunk(w, "pgen");

    for(i = 0; i < m->n_pgen; i++)
    {
        sfw_u16(w, m->pgen[i].id);
        sfw_u16(w, (unsigned short)m->pgen[i].amount);
    }

    sfw_zeros(w, SFW_GEN_SIZE); /* terminal record */
    sfw_end_chunk(w);

    /* instrument headers */
    sfw_begin_chunk(w, "inst");

    for(i = 0; i < m->n_inst; i++)
    {
        sfw_name(w, m->inst[i].name);
        sfw_u16(w, m->inst[i].bagndx);
    }

    sfw_zeros(w, 20); /* terminal record */
    sfw_u16(w, m->n_ibag);
    sfw_end_chunk(w);

    /* instrument zone indices */
    sfw_begin_chunk(w, "ibag");

    for(i = 0; i < m->n_ibag; i++)
    {
        sfw_u16(w, m->ibag[i].genndx);
        sfw_u16(w, m->ibag[i].modndx);
    }

    sfw_u16(w, m->n_igen); /* terminal record */
    sfw_u16(w, m->n_imod);
    sfw_end_chunk(w);

    /* instrument modulators */
    sfw_begin_chunk(w, "imod");

    for(i = 0; i < m->n_imod; i++)
    {
        sfw_write_mod_rec(w, &m->imod[i]);
    }

    sfw_zeros(w, SFW_MOD_SIZE); /* terminal record */
    sfw_end_chunk(w);

    /* instrument generators */
    sfw_begin_chunk(w, "igen");

    for(i = 0; i < m->n_igen; i++)
    {
        sfw_u16(w, m->igen[i].id);
        sfw_u16(w, (unsigned short)m->igen[i].amount);
    }

    sfw_zeros(w, SFW_GEN_SIZE); /* terminal record */
    sfw_end_chunk(w);

    /* sample headers */
    sfw_begin_chunk(w, "shdr");

    for(i = 0; i < m->n_shdr; i++)
    {
        sfw_name(w, m->shdr[i].name);
        sfw_u32(w, m->shdr[i].start);
        sfw_u32(w, m->shdr[i].end);
        sfw_u32(w, m->shdr[i].loopstart);
        sfw_u32(w, m->shdr[i].loopend);
        sfw_u32(w, m->shdr[i].samplerate);
        sfw_u8(w, (unsigned int)m->shdr[i].origpitch);
        sfw_u8(w, (unsigned int)(m->shdr[i].pitchadj & 0xFF));
        sfw_u16(w, 0); /* sample link */
        sfw_u16(w, m->shdr[i].sampletype);
    }

    sfw_zeros(w, SFW_SHDR_SIZE); /* terminal record */
    sfw_end_chunk(w);

    sfw_end_chunk(w); /* LIST pdta */
}

static int sfw_write_file(fluid_defsfont_t *defsfont, const sfw_model_t *m, FILE *fp)
{
    sfw_writer_t w;

    memset(&w, 0, sizeof(w));
    w.fp = fp;

    sfw_begin_list(&w, "RIFF", "sfbk");

    sfw_write_info(&w, defsfont);

    sfw_begin_list(&w, "LIST", "sdta");
    sfw_begin_chunk(&w, "smpl");
    sfw_write(&w, m->smpl, m->smpl_len);
    sfw_end_chunk(&w);
    sfw_end_chunk(&w); /* LIST sdta */

    sfw_write_pdta(&w, m);

    sfw_end_chunk(&w); /* RIFF sfbk */

    if(w.err || w.level != 0)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: failed while writing the output file");
        return FLUID_FAILED;
    }

    return FLUID_OK;
}

/*--------------------------------------------------------------------------
 * Public entry points
 *--------------------------------------------------------------------------*/
int fluid_defsfont_export_sf3(fluid_defsfont_t *defsfont, const char *filename)
{
    sfw_model_t model;
    FILE *fp;
    int ret = FLUID_FAILED;

    fluid_return_val_if_fail(defsfont != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(filename != NULL, FLUID_FAILED);

    memset(&model, 0, sizeof(model));

    /* samples first (fills the smpl data and shdr table), then instruments
     * (reference samples by index), then presets (reference instruments) */
    if(sfw_build_samples(defsfont, &model) == FLUID_FAILED)
    {
        goto out;
    }

    if(sfw_build_instruments(defsfont, &model) == FLUID_FAILED)
    {
        goto out;
    }

    if(sfw_build_presets(defsfont, &model) == FLUID_FAILED)
    {
        goto out;
    }

    fp = FLUID_FOPEN(filename, "wb");

    if(fp == NULL)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: unable to open '%s' for writing", filename);
        goto out;
    }

    if(sfw_write_file(defsfont, &model, fp) == FLUID_FAILED || fclose(fp) != 0)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export: failed to write '%s'", filename);
        goto out;
    }

    ret = FLUID_OK;

out:
    sfw_model_free(&model);

    if(ret == FLUID_FAILED)
    {
        remove(filename);
    }

    return ret;
}

#else  /* LIBSNDFILE_SUPPORT */

int fluid_defsfont_export_sf3(fluid_defsfont_t *defsfont, const char *filename)
{
    fluid_return_val_if_fail(defsfont != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(filename != NULL, FLUID_FAILED);
    FLUID_LOG(FLUID_ERR, "SF3 export requires libsndfile with Ogg Vorbis support");
    return FLUID_FAILED;
}
#endif /* LIBSNDFILE_SUPPORT */

/**
 * Export a SoundFont as a SoundFont 3 (SF3) file.
 *
 * The complete current state of the SoundFont is serialized: presets,
 * instruments, zones, generators (including any runtime modifications),
 * modulators and samples. All samples are compressed as Ogg Vorbis streams
 * as mandated by the SF3 format.
 *
 * @param sfont SoundFont to export (must have been loaded by the default
 *   SoundFont loader)
 * @param filename Path of the SF3 file to create
 * @return #FLUID_OK on success, #FLUID_FAILED otherwise
 * @since 2.7.0
 */
int fluid_sfont_export_sf3(fluid_sfont_t *sfont, const char *filename)
{
    fluid_return_val_if_fail(sfont != NULL, FLUID_FAILED);
    fluid_return_val_if_fail(filename != NULL, FLUID_FAILED);

    if(sfont->free != fluid_defsfont_sfont_delete)
    {
        FLUID_LOG(FLUID_ERR, "SF3 export is only supported for SoundFonts loaded by the default SoundFont loader");
        return FLUID_FAILED;
    }

    return fluid_defsfont_export_sf3((fluid_defsfont_t *)fluid_sfont_get_data(sfont), filename);
}
