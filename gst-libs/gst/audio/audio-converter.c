/* GStreamer
 * Copyright (C) 2005 Wim Taymans <wim at fluendo dot com>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * audioconverter.c: Convert audio to different audio formats automatically
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <string.h>

#include "audio-converter.h"
#include "gstaudiopack.h"

/**
 * SECTION:audioconverter
 * @short_description: Generic audio conversion
 *
 * <refsect2>
 * <para>
 * This object is used to convert audio samples from one format to another.
 * The object can perform conversion of:
 * <itemizedlist>
 *  <listitem><para>
 *    audio format with optional dithering and noise shaping
 *  </para></listitem>
 *  <listitem><para>
 *    audio samplerate
 *  </para></listitem>
 *  <listitem><para>
 *    audio channels and channel layout
 *  </para></listitem>
 * </para>
 * </refsect2>
 */

#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT ensure_debug_category()
static GstDebugCategory *
ensure_debug_category (void)
{
  static gsize cat_gonce = 0;

  if (g_once_init_enter (&cat_gonce)) {
    gsize cat_done;

    cat_done = (gsize) _gst_debug_category_new ("audio-converter", 0,
        "audio-converter object");

    g_once_init_leave (&cat_gonce, cat_done);
  }

  return (GstDebugCategory *) cat_gonce;
}
#else
#define ensure_debug_category() /* NOOP */
#endif /* GST_DISABLE_GST_DEBUG */

typedef struct _AudioChain AudioChain;

typedef void (*AudioConvertFunc) (gpointer dst, const gpointer src, gint count);
/*                           int/int    int/float  float/int float/float
 *
 *  unpack                     S32          S32         F64       F64
 *  convert                               S32->F64
 *  channel mix                S32          F64         F64       F64
 *  convert                                           F64->S32
 *  quantize                   S32                      S32
 *  pack                       S32          F64         S32       F64
 *
 *
 *  interleave
 *  deinterleave
 *  resample
 */
struct _GstAudioConverter
{
  GstAudioInfo in;
  GstAudioInfo out;

  GstStructure *config;

  GstAudioConverterFlags flags;
  GstAudioFormat current_format;
  GstAudioLayout current_layout;
  gint current_channels;

  gpointer *in_data;
  gpointer *out_data;

  /* unpack */
  gboolean in_default;
  gboolean unpack_ip;
  AudioChain *unpack_chain;

  /* convert in */
  AudioConvertFunc convert_in;
  AudioChain *convert_in_chain;

  /* channel mix */
  gboolean mix_passthrough;
  GstAudioChannelMix *mix;
  AudioChain *mix_chain;

  /* convert out */
  AudioConvertFunc convert_out;
  AudioChain *convert_out_chain;

  /* quant */
  GstAudioQuantize *quant;
  AudioChain *quant_chain;

  /* pack */
  gboolean out_default;
  AudioChain *pack_chain;

  gboolean passthrough;
};

typedef gboolean (*AudioChainFunc) (AudioChain * chain, gsize samples,
    gpointer user_data);
typedef gpointer *(*AudioChainAllocFunc) (AudioChain * chain, gsize samples,
    gpointer user_data);

static gpointer *get_output_samples (AudioChain * chain, gsize samples,
    gpointer user_data);

struct _AudioChain
{
  AudioChain *prev;

  AudioChainFunc make_func;
  gpointer make_func_data;
  GDestroyNotify make_func_notify;

  gint stride;
  gint inc;
  gint blocks;

  gboolean pass_alloc;
  gboolean allow_ip;

  AudioChainAllocFunc alloc_func;
  gpointer alloc_data;

  gpointer *tmp;
  gsize tmpsize;

  gpointer *samples;
};

static AudioChain *
audio_chain_new (AudioChain * prev, GstAudioConverter * convert)
{
  AudioChain *chain;
  const GstAudioFormatInfo *finfo;

  chain = g_slice_new0 (AudioChain);
  chain->prev = prev;

  if (convert->current_layout == GST_AUDIO_LAYOUT_NON_INTERLEAVED) {
    chain->inc = 1;
    chain->blocks = convert->current_channels;
  } else {
    chain->inc = convert->current_channels;
    chain->blocks = 1;
  }
  finfo = gst_audio_format_get_info (convert->current_format);
  chain->stride = (finfo->width * chain->inc) / 8;

  return chain;
}

static void
audio_chain_set_make_func (AudioChain * chain,
    AudioChainFunc make_func, gpointer user_data, GDestroyNotify notify)
{
  chain->make_func = make_func;
  chain->make_func_data = user_data;
  chain->make_func_notify = notify;
}

static void
audio_chain_free (AudioChain * chain)
{
  GST_LOG ("free chain %p", chain);
  if (chain->make_func_notify)
    chain->make_func_notify (chain->make_func_data);
  g_free (chain->tmp);
  g_slice_free (AudioChain, chain);
}

static gpointer *
audio_chain_alloc_samples (AudioChain * chain, guint samples)
{
  return chain->alloc_func (chain, samples, chain->alloc_data);
}

static gpointer *
audio_chain_get_samples (AudioChain * chain, guint samples)
{
  gpointer *res;

  while (!chain->samples)
    chain->make_func (chain, samples, chain->make_func_data);

  res = chain->samples;
  chain->samples = NULL;

  return res;
}

/*
static guint
get_opt_uint (GstAudioConverter * convert, const gchar * opt, guint def)
{
  guint res;
  if (!gst_structure_get_uint (convert->config, opt, &res))
    res = def;
  return res;
}
*/

static gint
get_opt_enum (GstAudioConverter * convert, const gchar * opt, GType type,
    gint def)
{
  gint res;
  if (!gst_structure_get_enum (convert->config, opt, type, &res))
    res = def;
  return res;
}

#define DEFAULT_OPT_DITHER_METHOD GST_AUDIO_DITHER_NONE
#define DEFAULT_OPT_NOISE_SHAPING_METHOD GST_AUDIO_NOISE_SHAPING_NONE
#define DEFAULT_OPT_QUANTIZATION 1

#define GET_OPT_DITHER_METHOD(c) get_opt_enum(c, \
    GST_AUDIO_CONVERTER_OPT_DITHER_METHOD, GST_TYPE_AUDIO_DITHER_METHOD, \
    DEFAULT_OPT_DITHER_METHOD)
#define GET_OPT_NOISE_SHAPING_METHOD(c) get_opt_enum(c, \
    GST_AUDIO_CONVERTER_OPT_NOISE_SHAPING_METHOD, GST_TYPE_AUDIO_NOISE_SHAPING_METHOD, \
    DEFAULT_OPT_NOISE_SHAPING_METHOD)
#define GET_OPT_QUANTIZATION(c) get_opt_uint(c, \
    GST_AUDIO_CONVERTER_OPT_QUANTIZATION, DEFAULT_OPT_QUANTIZATION)

static gboolean
copy_config (GQuark field_id, const GValue * value, gpointer user_data)
{
  GstAudioConverter *convert = user_data;

  gst_structure_id_set_value (convert->config, field_id, value);

  return TRUE;
}

/**
 * gst_audio_converter_set_config:
 * @convert: a #GstAudioConverter
 * @config: (transfer full): a #GstStructure
 *
 * Set @config as extra configuraion for @convert.
 *
 * If the parameters in @config can not be set exactly, this function returns
 * %FALSE and will try to update as much state as possible. The new state can
 * then be retrieved and refined with gst_audio_converter_get_config().
 *
 * Look at the #GST_AUDIO_CONVERTER_OPT_* fields to check valid configuration
 * option and values.
 *
 * Returns: %TRUE when @config could be set.
 */
gboolean
gst_audio_converter_set_config (GstAudioConverter * convert,
    GstStructure * config)
{
  g_return_val_if_fail (convert != NULL, FALSE);
  g_return_val_if_fail (config != NULL, FALSE);

  gst_structure_foreach (config, copy_config, convert);
  gst_structure_free (config);

  return TRUE;
}

/**
 * gst_audio_converter_get_config:
 * @convert: a #GstAudioConverter
 *
 * Get the current configuration of @convert.
 *
 * Returns: a #GstStructure that remains valid for as long as @convert is valid
 *   or until gst_audio_converter_set_config() is called.
 */
const GstStructure *
gst_audio_converter_get_config (GstAudioConverter * convert)
{
  g_return_val_if_fail (convert != NULL, NULL);

  return convert->config;
}

static gboolean
do_unpack (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;
  gpointer *tmp;
  gboolean src_writable;

  src_writable = (convert->flags & GST_AUDIO_CONVERTER_FLAG_SOURCE_WRITABLE);

  if (!chain->allow_ip || !src_writable || !convert->in_default) {
    gint i;

    if (src_writable && chain->allow_ip)
      tmp = convert->in_data;
    else
      tmp = audio_chain_alloc_samples (chain, samples);
    GST_LOG ("unpack %p %p, %" G_GSIZE_FORMAT, tmp, convert->in_data, samples);

    for (i = 0; i < chain->blocks; i++) {
      convert->in.finfo->unpack_func (convert->in.finfo,
          GST_AUDIO_PACK_FLAG_TRUNCATE_RANGE, tmp[i], convert->in_data[i],
          samples * chain->inc);
    }
  } else {
    tmp = convert->in_data;
    GST_LOG ("get in samples %p", tmp);
  }
  chain->samples = tmp;

  return TRUE;
}

static gboolean
do_convert_in (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;
  gpointer *in, *out;
  gint i;

  in = audio_chain_get_samples (chain->prev, samples);
  out = (chain->allow_ip ? in : audio_chain_alloc_samples (chain, samples));
  GST_LOG ("convert in %p, %p %" G_GSIZE_FORMAT, in, out, samples);

  for (i = 0; i < chain->blocks; i++)
    convert->convert_in (out[i], in[i], samples * chain->inc);

  chain->samples = out;

  return TRUE;
}

static gboolean
do_mix (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;
  gpointer *in, *out;

  in = audio_chain_get_samples (chain->prev, samples);
  out = (chain->allow_ip ? in : audio_chain_alloc_samples (chain, samples));
  GST_LOG ("mix %p %p,%" G_GSIZE_FORMAT, in, out, samples);

  gst_audio_channel_mix_samples (convert->mix, in, out, samples);

  chain->samples = out;

  return TRUE;
}

static gboolean
do_convert_out (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;
  gpointer *in, *out;
  gint i;

  in = audio_chain_get_samples (chain->prev, samples);
  out = (chain->allow_ip ? in : audio_chain_alloc_samples (chain, samples));
  GST_LOG ("convert out %p, %p %" G_GSIZE_FORMAT, in, out, samples);

  for (i = 0; i < chain->blocks; i++)
    convert->convert_out (out[i], in[i], samples * chain->inc);

  chain->samples = out;

  return TRUE;
}

static gboolean
do_quantize (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;
  gpointer *in, *out;

  in = audio_chain_get_samples (chain->prev, samples);
  out = (chain->allow_ip ? in : audio_chain_alloc_samples (chain, samples));
  GST_LOG ("quantize %p, %p %" G_GSIZE_FORMAT, in, out, samples);

  gst_audio_quantize_samples (convert->quant, in, out, samples);

  chain->samples = out;

  return TRUE;
}

static AudioChain *
chain_unpack (GstAudioConverter * convert)
{
  AudioChain *prev;
  GstAudioInfo *in = &convert->in;
  const GstAudioFormatInfo *fup;

  convert->current_format = in->finfo->unpack_format;
  convert->current_layout = in->layout;
  convert->current_channels = in->channels;
  convert->in_default = in->finfo->unpack_format == in->finfo->format;

  GST_INFO ("unpack format %s to %s",
      gst_audio_format_to_string (in->finfo->format),
      gst_audio_format_to_string (convert->current_format));

  fup = gst_audio_format_get_info (in->finfo->unpack_format);

  prev = convert->unpack_chain = audio_chain_new (NULL, convert);
  prev->allow_ip = fup->width <= in->finfo->width;
  prev->pass_alloc = FALSE;
  audio_chain_set_make_func (prev, do_unpack, convert, NULL);

  return prev;
}

static AudioChain *
chain_convert_in (GstAudioConverter * convert, AudioChain * prev)
{
  gboolean in_int, out_int;
  GstAudioInfo *in = &convert->in;
  GstAudioInfo *out = &convert->out;

  in_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (in->finfo);
  out_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (out->finfo);

  if (in_int && !out_int) {
    GST_INFO ("convert S32 to F64");
    convert->convert_in = (AudioConvertFunc) audio_orc_s32_to_double;
    convert->current_format = GST_AUDIO_FORMAT_F64;

    prev = convert->convert_in_chain = audio_chain_new (prev, convert);
    prev->allow_ip = FALSE;
    prev->pass_alloc = FALSE;
    audio_chain_set_make_func (prev, do_convert_in, convert, NULL);
  }
  return prev;
}

static AudioChain *
chain_mix (GstAudioConverter * convert, AudioChain * prev)
{
  GstAudioChannelMixFlags flags;
  GstAudioInfo *in = &convert->in;
  GstAudioInfo *out = &convert->out;
  GstAudioFormat format = convert->current_format;

  flags =
      GST_AUDIO_INFO_IS_UNPOSITIONED (in) ?
      GST_AUDIO_CHANNEL_MIX_FLAGS_UNPOSITIONED_IN : 0;
  flags |=
      GST_AUDIO_INFO_IS_UNPOSITIONED (out) ?
      GST_AUDIO_CHANNEL_MIX_FLAGS_UNPOSITIONED_OUT : 0;

  convert->current_channels = out->channels;

  convert->mix =
      gst_audio_channel_mix_new (flags, format, in->channels, in->position,
      out->channels, out->position);
  convert->mix_passthrough =
      gst_audio_channel_mix_is_passthrough (convert->mix);
  GST_INFO ("mix format %s, passthrough %d, in_channels %d, out_channels %d",
      gst_audio_format_to_string (format), convert->mix_passthrough,
      in->channels, out->channels);

  if (!convert->mix_passthrough) {
    prev = convert->mix_chain = audio_chain_new (prev, convert);
    /* we can only do in-place when in >= out, else we don't have enough
     * memory. */
    prev->allow_ip = in->channels >= out->channels;
    prev->pass_alloc = in->channels <= out->channels;
    audio_chain_set_make_func (prev, do_mix, convert, NULL);
  }
  return prev;
}

static AudioChain *
chain_convert_out (GstAudioConverter * convert, AudioChain * prev)
{
  gboolean in_int, out_int;
  GstAudioInfo *in = &convert->in;
  GstAudioInfo *out = &convert->out;

  in_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (in->finfo);
  out_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (out->finfo);

  if (!in_int && out_int) {
    convert->convert_out = (AudioConvertFunc) audio_orc_double_to_s32;
    convert->current_format = GST_AUDIO_FORMAT_S32;

    GST_INFO ("convert F64 to S32");
    prev = convert->convert_out_chain = audio_chain_new (prev, convert);
    prev->allow_ip = TRUE;
    prev->pass_alloc = FALSE;
    audio_chain_set_make_func (prev, do_convert_out, convert, NULL);
  }
  return prev;
}

static AudioChain *
chain_quantize (GstAudioConverter * convert, AudioChain * prev)
{
  GstAudioInfo *in = &convert->in;
  GstAudioInfo *out = &convert->out;
  gint in_depth, out_depth;
  gboolean in_int, out_int;
  GstAudioDitherMethod dither;
  GstAudioNoiseShapingMethod ns;

  dither = GET_OPT_DITHER_METHOD (convert);
  ns = GET_OPT_NOISE_SHAPING_METHOD (convert);

  in_depth = GST_AUDIO_FORMAT_INFO_DEPTH (in->finfo);
  out_depth = GST_AUDIO_FORMAT_INFO_DEPTH (out->finfo);
  GST_INFO ("depth in %d, out %d", in_depth, out_depth);

  in_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (in->finfo);
  out_int = GST_AUDIO_FORMAT_INFO_IS_INTEGER (out->finfo);

  /* Don't dither or apply noise shaping if target depth is bigger than 20 bits
   * as DA converters only can do a SNR up to 20 bits in reality.
   * Also don't dither or apply noise shaping if target depth is larger than
   * source depth. */
  if (out_depth > 20 || (in_int && out_depth >= in_depth)) {
    dither = GST_AUDIO_DITHER_NONE;
    ns = GST_AUDIO_NOISE_SHAPING_NONE;
    GST_INFO ("using no dither and noise shaping");
  } else {
    GST_INFO ("using dither %d and noise shaping %d", dither, ns);
    /* Use simple error feedback when output sample rate is smaller than
     * 32000 as the other methods might move the noise to audible ranges */
    if (ns > GST_AUDIO_NOISE_SHAPING_ERROR_FEEDBACK && out->rate < 32000)
      ns = GST_AUDIO_NOISE_SHAPING_ERROR_FEEDBACK;
  }
  /* we still want to run the quantization step when reducing bits to get
   * the rounding correct */
  if (out_int && out_depth < 32) {
    GST_INFO ("quantize to %d bits, dither %d, ns %d", out_depth, dither, ns);
    convert->quant =
        gst_audio_quantize_new (dither, ns, 0, convert->current_format,
        out->channels, 1U << (32 - out_depth));

    prev = convert->quant_chain = audio_chain_new (prev, convert);
    prev->allow_ip = TRUE;
    prev->pass_alloc = TRUE;
    audio_chain_set_make_func (prev, do_quantize, convert, NULL);
  }
  return prev;
}

static AudioChain *
chain_pack (GstAudioConverter * convert, AudioChain * prev)
{
  GstAudioInfo *out = &convert->out;
  GstAudioFormat format = convert->current_format;

  convert->current_format = out->finfo->format;

  g_assert (out->finfo->unpack_format == format);
  convert->out_default = format == out->finfo->format;
  GST_INFO ("pack format %s to %s", gst_audio_format_to_string (format),
      gst_audio_format_to_string (out->finfo->format));

  return prev;
}

static gpointer *
get_output_samples (AudioChain * chain, gsize samples, gpointer user_data)
{
  GstAudioConverter *convert = user_data;

  GST_LOG ("output samples %" G_GSIZE_FORMAT, samples);
  return convert->out_data;
}

static gpointer *
get_temp_samples (AudioChain * chain, gsize samples, gpointer user_data)
{
  gsize needed;

  /* first part contains the pointers, second part the data */
  needed = (samples * chain->stride + sizeof (gpointer)) * chain->blocks;

  if (needed > chain->tmpsize) {
    gint i;
    guint8 *s;

    GST_DEBUG ("alloc samples %" G_GSIZE_FORMAT, needed);
    chain->tmp = g_realloc (chain->tmp, needed);
    chain->tmpsize = needed;

    /* jump to the data */
    s = (guint8 *) & chain->tmp[chain->blocks];

    /* set up the pointers */
    for (i = 0; i < chain->blocks; i++)
      chain->tmp[i] = s + (i * samples * chain->stride);
  }
  return chain->tmp;
}

static void
setup_allocators (GstAudioConverter * convert)
{
  AudioChain *chain;
  AudioChainAllocFunc alloc_func;
  gboolean allow_ip;

  /* start with using dest if we can directly write into it */
  if (convert->out_default) {
    alloc_func = get_output_samples;
    allow_ip = FALSE;
  } else {
    alloc_func = get_temp_samples;
    allow_ip = TRUE;
  }
  /* now walk backwards, we try to write into the dest samples directly
   * and keep track if the source needs to be writable */
  for (chain = convert->pack_chain; chain; chain = chain->prev) {
    chain->alloc_func = alloc_func;
    chain->alloc_data = convert;
    chain->allow_ip = allow_ip && chain->allow_ip;

    if (!chain->pass_alloc) {
      /* can't pass allocator, make new temp line allocator */
      alloc_func = get_temp_samples;
      allow_ip = TRUE;
    }
  }
}

/**
 * gst_audio_converter_new: (skip)
 * @in: a source #GstAudioInfo
 * @out: a destination #GstAudioInfo
 * @config: (transfer full): a #GstStructure with configuration options
 *
 * Create a new #GstAudioConverter that is able to convert between @in and @out
 * audio formats.
 *
 * @config contains extra configuration options, see #GST_VIDEO_CONVERTER_OPT_*
 * parameters for details about the options and values.
 *
 * Returns: a #GstAudioConverter or %NULL if conversion is not possible.
 */
GstAudioConverter *
gst_audio_converter_new (GstAudioInfo * in, GstAudioInfo * out,
    GstStructure * config)
{
  GstAudioConverter *convert;
  AudioChain *prev;

  g_return_val_if_fail (in != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (in->rate == out->rate, FALSE);
  g_return_val_if_fail (in->layout == GST_AUDIO_LAYOUT_INTERLEAVED, FALSE);
  g_return_val_if_fail (in->layout == out->layout, FALSE);

  if ((GST_AUDIO_INFO_CHANNELS (in) != GST_AUDIO_INFO_CHANNELS (out)) &&
      (GST_AUDIO_INFO_IS_UNPOSITIONED (in)
          || GST_AUDIO_INFO_IS_UNPOSITIONED (out)))
    goto unpositioned;

  convert = g_slice_new0 (GstAudioConverter);

  convert->in = *in;
  convert->out = *out;

  /* default config */
  convert->config = gst_structure_new_empty ("GstAudioConverter");
  if (config)
    gst_audio_converter_set_config (convert, config);

  GST_INFO ("unitsizes: %d -> %d", in->bpf, out->bpf);

  /* step 1, unpack */
  prev = chain_unpack (convert);
  /* step 2, optional convert from S32 to F64 for channel mix */
  prev = chain_convert_in (convert, prev);
  /* step 3, channel mix */
  prev = chain_mix (convert, prev);
  /* step 4, optional convert for quantize */
  prev = chain_convert_out (convert, prev);
  /* step 5, optional quantize */
  prev = chain_quantize (convert, prev);
  /* step 6, pack */
  convert->pack_chain = chain_pack (convert, prev);

  /* optimize */
  if (out->finfo->format == in->finfo->format && convert->mix_passthrough) {
    GST_INFO ("same formats and passthrough mixing -> passthrough");
    convert->passthrough = TRUE;
  }

  setup_allocators (convert);


  return convert;

  /* ERRORS */
unpositioned:
  {
    GST_WARNING ("unpositioned channels");
    return NULL;
  }
}

/**
 * gst_audio_converter_free:
 * @convert: a #GstAudioConverter
 *
 * Free a previously allocated @convert instance.
 */
void
gst_audio_converter_free (GstAudioConverter * convert)
{
  g_return_if_fail (convert != NULL);

  if (convert->unpack_chain)
    audio_chain_free (convert->unpack_chain);
  if (convert->convert_in_chain)
    audio_chain_free (convert->convert_in_chain);
  if (convert->mix_chain)
    audio_chain_free (convert->mix_chain);
  if (convert->convert_out_chain)
    audio_chain_free (convert->convert_out_chain);
  if (convert->quant_chain)
    audio_chain_free (convert->quant_chain);

  if (convert->quant)
    gst_audio_quantize_free (convert->quant);
  if (convert->mix)
    gst_audio_channel_mix_free (convert->mix);
  gst_audio_info_init (&convert->in);
  gst_audio_info_init (&convert->out);

  gst_structure_free (convert->config);

  g_slice_free (GstAudioConverter, convert);
}

/**
 * gst_audio_converter_get_out_frames:
 * @convert: a #GstAudioConverter
 * @in_frames: number of input frames
 *
 * Calculate how many output frames can be produced when @in_frames input
 * frames are given to @convert.
 *
 * Returns: the number of output frames
 */
gsize
gst_audio_converter_get_out_frames (GstAudioConverter * convert,
    gsize in_frames)
{
  return in_frames;
}

/**
 * gst_audio_converter_get_in_frames:
 * @convert: a #GstAudioConverter
 * @out_frames: number of output frames
 *
 * Calculate how many input frames are currently needed by @convert to produce
 * @out_frames of output frames.
 *
 * Returns: the number of input frames
 */
gsize
gst_audio_converter_get_in_frames (GstAudioConverter * convert,
    gsize out_frames)
{
  return out_frames;
}

/**
 * gst_audio_converter_get_max_latency:
 * @convert: a #GstAudioConverter
 *
 * Get the maximum number of input frames that the converter would
 * need before producing output.
 *
 * Returns: the latency of @convert as expressed in the number of
 * frames.
 */
gsize
gst_audio_converter_get_max_latency (GstAudioConverter * convert)
{
  return 0;
}

/**
 * gst_audio_converter_samples:
 * @convert: a #GstAudioConverter
 * @flags: extra #GstAudioConverterFlags
 * @in: input samples
 * @in_samples: number of input samples
 * @out: output samples
 * @out_samples: number of output samples
 * @in_consumed: number of input samples consumed
 * @out_produced: number of output samples produced
 *
 * Perform the conversion with @in_samples in @in to @out_samples in @out
 * using @convert.
 *
 * In case the samples are interleaved, @in and @out must point to an
 * array with a single element pointing to a block of interleaved samples.
 *
 * If non-interleaved samples are used, @in and @out must point to an
 * array with pointers to memory blocks, one for each channel.
 *
 * The actual number of samples used from @in is returned in @in_consumed and
 * can be less than @in_samples. The actual number of samples produced is
 * returned in @out_produced and can be less than @out_samples.
 *
 * Returns: %TRUE is the conversion could be performed.
 */
gboolean
gst_audio_converter_samples (GstAudioConverter * convert,
    GstAudioConverterFlags flags, gpointer in[], gsize in_samples,
    gpointer out[], gsize out_samples, gsize * in_consumed,
    gsize * out_produced)
{
  AudioChain *chain;
  gpointer *tmp;
  gint i;

  g_return_val_if_fail (convert != NULL, FALSE);
  g_return_val_if_fail (in != NULL, FALSE);
  g_return_val_if_fail (out != NULL, FALSE);
  g_return_val_if_fail (in_consumed != NULL, FALSE);
  g_return_val_if_fail (out_produced != NULL, FALSE);

  in_samples = MIN (in_samples, out_samples);

  if (in_samples == 0) {
    *in_consumed = 0;
    *out_produced = 0;
    return TRUE;
  }

  chain = convert->pack_chain;

  if (convert->passthrough) {
    for (i = 0; i < chain->blocks; i++)
      memcpy (out[i], in[i], in_samples * chain->inc);
    *out_produced = in_samples;
    *in_consumed = in_samples;
    return TRUE;
  }

  convert->flags = flags;
  convert->in_data = in;
  convert->out_data = out;

  /* get samples to pack */
  tmp = audio_chain_get_samples (chain, in_samples);

  if (!convert->out_default) {
    GST_LOG ("pack %p, %p %" G_GSIZE_FORMAT, tmp, out, in_samples);
    /* and pack if needed */
    for (i = 0; i < chain->blocks; i++)
      convert->out.finfo->pack_func (convert->out.finfo, 0, tmp[i], out[i],
          in_samples * chain->inc);
  }

  *out_produced = in_samples;
  *in_consumed = in_samples;

  return TRUE;
}
