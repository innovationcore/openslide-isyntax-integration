/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

#include <openjpeg.h>

static const char APERIO_DESCRIPTION[] = "Aperio Image Library";

struct _openslide_tiff_tilereader {
  TIFF *tiff;
  int64_t tile_width;
  int64_t tile_height;
};

// OpenJPEG memory-read functions
struct _openslide_opj_mem_stream_state {
  const tdata_t buf;
  const tsize_t size;
  toff_t offset;
};

static OPJ_UINT32 _openslide_opj_mem_stream_read (void *p_buffer,
						  OPJ_UINT32 p_nb_bytes,
						  void *p_user_data) {
  struct _openslide_opj_mem_stream_state *ss =
    (struct _openslide_opj_mem_stream_state *) p_user_data;

  //  g_debug("READ: %p, %d", p_buffer, p_nb_bytes);

  if (ss->offset == ss->size) {
    return -1; // EOF
  }

  toff_t new_offset = ss->offset + p_nb_bytes;
  size_t bytes_to_read = p_nb_bytes;

  if (new_offset > ss->size) {
    toff_t over = new_offset - ss->size;
    bytes_to_read -= over;
    new_offset = ss->size;
  }

  memcpy(p_buffer, ((uint8_t *) ss->buf) + ss->offset, bytes_to_read);
  ss->offset = new_offset;

  return bytes_to_read;
}

static OPJ_SIZE_T _openslide_opj_mem_stream_skip (OPJ_SIZE_T p_nb_bytes,
						  void *p_user_data) {
  struct _openslide_opj_mem_stream_state *ss =
    (struct _openslide_opj_mem_stream_state *) p_user_data;

  if (ss->offset == ss->size) {
    return -1; // EOF
  }
  toff_t new_offset = ss->offset + p_nb_bytes;
  size_t bytes_to_skip = p_nb_bytes;

  if (new_offset > ss->size) {
    toff_t over = new_offset - ss->size;
    bytes_to_skip -= over;
    new_offset = ss->size;
  }

  ss->offset = new_offset;

  return bytes_to_skip;
}

static bool _openslide_opj_mem_stream_seek (OPJ_SIZE_T p_nb_bytes,
					    void *p_user_data) {
  struct _openslide_opj_mem_stream_state *ss =
    (struct _openslide_opj_mem_stream_state *) p_user_data;

  if (p_nb_bytes > ss->size) {
    return false;
  }

  ss->offset = p_nb_bytes;

  return true;
}

static void info_callback(const OPJ_CHAR *msg, void *data) {
  g_message("%s", msg);
}
static void warning_callback(const OPJ_CHAR *msg, void *data) {
  g_warning("%s", msg);
}
static void error_callback(const OPJ_CHAR *msg, void *data) {
  g_critical("%s", msg);
}

// XXX revisit assumptions that color is always downsampled in x by 2
static struct _openslide_tiff_tilereader *_openslide_aperio_tiff_tilereader_create(TIFF *tiff) {
  struct _openslide_tiff_tilereader *wtt = g_slice_new(struct _openslide_tiff_tilereader);
  uint32_t tmp;

  wtt->tiff = tiff;

  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp);
  wtt->tile_width = tmp;
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp);
  wtt->tile_height = tmp;

  return wtt;
}

static void _openslide_aperio_tiff_tilereader_read(struct _openslide_tiff_tilereader *wtt,
						   uint32_t *dest,
						   int64_t x, int64_t y) {
  // get tile number
  ttile_t tile_no = TIFFComputeTile(wtt->tiff, x, y, 0, 0);

  //  g_debug("aperio reading tile_no: %d", tile_no);

  // get tile size
  tsize_t max_tile_size = TIFFTileSize(wtt->tiff);

  // get raw tile
  tdata_t buf = g_slice_alloc(max_tile_size);
  tsize_t size = TIFFReadRawTile(wtt->tiff, tile_no, buf, max_tile_size); // XXX?

  // set source of compressed data
  struct _openslide_opj_mem_stream_state stream_state = {
    .buf = buf,
    .size = size,
    .offset = 0,
  };
  opj_stream_t *stream = opj_stream_default_create(true);
  opj_stream_set_read_function(stream, _openslide_opj_mem_stream_read);
  opj_stream_set_skip_function(stream, _openslide_opj_mem_stream_skip);
  opj_stream_set_seek_function(stream, _openslide_opj_mem_stream_seek);
  opj_stream_set_user_data(stream, &stream_state);

  // decode
  OPJ_INT32 tx0;
  OPJ_INT32 ty0;
  OPJ_UINT32 tw;
  OPJ_UINT32 th;
  OPJ_UINT32 ntx;
  OPJ_UINT32 nty;
  opj_codec_t *codec = opj_create_decompress(CODEC_J2K);

  opj_set_info_handler(codec, info_callback, NULL);
  opj_set_warning_handler(codec, warning_callback, NULL);
  opj_set_error_handler(codec, error_callback, NULL);

  opj_image_t *image;
  opj_read_header(codec, &image,
		  &tx0, &ty0, &tw, &th, &ntx, &nty, stream);


  image = opj_decode(codec, stream);
  opj_end_decompress(codec, stream);
  opj_image_comp_t *comps = image->comps;

  // copy
  for (int i = 0; i < wtt->tile_height * wtt->tile_width; i++) {
    uint8_t Y = comps[0].data[i];
    uint8_t Cb = comps[1].data[i/2];
    uint8_t Cr = comps[2].data[i/2];

    uint8_t A = 255;
    double R = Y + 1.402 * (Cr - 128);
    double G = Y - 0.34414 * (Cb - 128) - 0.71414 * (Cr - 128);
    double B = Y + 1.772 * (Cb - 128);

    if (R > 255) {
      R = 255;
    }
    if (R < 0) {
      R = 0;
    }
    if (G > 255) {
      G = 255;
    }
    if (G < 0) {
      G = 0;
    }
    if (B > 255) {
      B = 255;
    }
    if (B < 0) {
      B = 0;
    }

    dest[i] = A << 24 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
  }

  // erase
  g_slice_free1(max_tile_size, buf);
  opj_image_destroy(image);
  opj_stream_destroy(stream);
  opj_destroy_codec(codec);
}

static void _openslide_aperio_tiff_tilereader_destroy(struct _openslide_tiff_tilereader *wtt) {
  g_slice_free(struct _openslide_tiff_tilereader, wtt);
}



bool _openslide_try_aperio(openslide_t *osr, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not aperio
  }

  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
    // not aperio
    TIFFClose(tiff);
    return false;
  }

  // for aperio, the tiled layers are the ones we want
  int32_t layer_count = 0;
  int32_t *layers = NULL;
  do {
    if (TIFFIsTiled(tiff)) {
      layer_count++;
    }
  } while (TIFFReadDirectory(tiff));
  layers = g_new(int32_t, layer_count);

  TIFFSetDirectory(tiff, 0);
  int32_t i = 0;
  do {
    if (TIFFIsTiled(tiff)) {
      layers[i++] = TIFFCurrentDirectory(tiff);
      g_debug("tiled layer: %d", TIFFCurrentDirectory(tiff));
    }
    TIFFReadDirectory(tiff);
  } while (i < layer_count);

  // all set, load up the TIFF-specific ops
  TIFFSetDirectory(tiff, 0);
  uint16_t compression_mode;
  TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression_mode);

  g_debug("compression mode: %d", compression_mode);

  if (compression_mode == 33003) {
    // special jpeg 2000 aperio thing
    _openslide_add_tiff_ops(osr, tiff, 0, NULL, layer_count, layers,
			    _openslide_aperio_tiff_tilereader_create,
			    _openslide_aperio_tiff_tilereader_read,
			    _openslide_aperio_tiff_tilereader_destroy);
  } else {
    // let libtiff handle it
    _openslide_add_tiff_ops(osr, tiff, 0, NULL, layer_count, layers,
			    _openslide_generic_tiff_tilereader_create,
			    _openslide_generic_tiff_tilereader_read,
			    _openslide_generic_tiff_tilereader_destroy);
  }

  return true;
}
