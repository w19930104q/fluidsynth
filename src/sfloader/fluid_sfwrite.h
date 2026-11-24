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


#ifndef _FLUID_SFWRITE_H
#define _FLUID_SFWRITE_H

#include "fluid_defsfont.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize a loaded SoundFont (including any runtime modifications made to
 * its presets, instruments, zones, generators and modulators) back to a
 * SoundFont 3 (SF3) file. All samples are compressed as Ogg Vorbis streams
 * in the smpl chunk.
 *
 * @param defsfont The SoundFont data to export
 * @param filename Path of the SF3 file to create
 * @return FLUID_OK on success, FLUID_FAILED otherwise
 */
int fluid_defsfont_export_sf3(fluid_defsfont_t *defsfont, const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* _FLUID_SFWRITE_H */
