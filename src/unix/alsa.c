/*         ______   ___    ___
 *        /\  _  \ /\_ \  /\_ \
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      ALSA sound driver.
 *
 *      By Peter Wang (based heavily on uoss.c)
 *
 *      ALSA 0.9 support added by Thomas Fjellstrom.
 *
 *      See readme.txt for copyright information.
 */


#include "allegro.h"

#if (defined DIGI_ALSA) && ((!defined ALLEGRO_WITH_MODULES) || (defined ALLEGRO_MODULE))

#include "allegro/internal/aintern.h"
#ifdef ALLEGRO_QNX
#include "allegro/platform/aintqnx.h"
#else
#include "allegro/platform/aintunix.h"
#endif

#ifndef SCAN_DEPEND
   #include <string.h>

   #if ALLEGRO_ALSA_VERSION == 9
      #define ALSA_PCM_NEW_HW_PARAMS_API 1
      #include <alsa/asoundlib.h>
      #include <math.h>
   #else  /* ALLEGRO_ALSA_VERSION == 5 */
      #include <sys/asoundlib.h>
   #endif

#endif


#if ALLEGRO_ALSA_VERSION == 9

#ifndef SND_PCM_FORMAT_S16_NE
   #ifdef ALLEGRO_BIG_ENDIAN
      #define SND_PCM_FORMAT_S16_NE SND_PCM_FORMAT_S16_BE
   #else
      #define SND_PCM_FORMAT_S16_NE SND_PCM_FORMAT_S16_LE
   #endif
#endif
#ifndef SND_PCM_FORMAT_U16_NE
   #ifdef ALLEGRO_BIG_ENDIAN
      #define SND_PCM_FORMAT_U16_NE SND_PCM_FORMAT_U16_BE
   #else
      #define SND_PCM_FORMAT_U16_NE SND_PCM_FORMAT_U16_LE
   #endif
#endif

#define ALSA9_CHECK(a) do { \
   int err = (a); \
   if (err<0) \
      uszprintf(allegro_error, ALLEGRO_ERROR_SIZE, "ALSA: %s : %s", #a, get_config_text(snd_strerror(err))); \
} while(0)


static char const *alsa_device = "default";
static snd_pcm_hw_params_t *hwparams = NULL;
static snd_pcm_sw_params_t *swparams = NULL;
static snd_pcm_channel_area_t *areas = NULL;
static snd_output_t *snd_output = NULL;
static snd_pcm_uframes_t alsa_bufsize;
static snd_mixer_t *alsa_mixer = NULL;
static snd_mixer_elem_t *alsa_mixer_elem = NULL;
static long alsa_mixer_elem_min, alsa_mixer_elem_max;
static double alsa_mixer_allegro_ratio = 0.0;

#else /* ALLEGRO_ALSA_VERSION == 5 */

#ifndef SND_PCM_SFMT_S16_NE
   #ifdef ALLEGRO_BIG_ENDIAN
      #define SND_PCM_SFMT_S16_NE SND_PCM_SFMT_S16_BE
   #else
      #define SND_PCM_SFMT_S16_NE SND_PCM_SFMT_S16_LE
   #endif
#endif
#ifndef SND_PCM_SFMT_U16_NE
   #ifdef ALLEGRO_BIG_ENDIAN
      #define SND_PCM_SFMT_U16_NE SND_PCM_SFMT_U16_BE
   #else
      #define SND_PCM_SFMT_U16_NE SND_PCM_SFMT_U16_LE
   #endif
#endif


static int alsa_bufsize;

#endif


#define ALSA_DEFAULT_NUMFRAGS   16


static snd_pcm_t *pcm_handle;
static unsigned char *alsa_bufdata;
static int alsa_bits, alsa_signed, alsa_rate, alsa_stereo;
static int alsa_fragments;

static char alsa_desc[256] = EMPTY_STRING;



static int alsa_detect(int input);
static int alsa_init(int input, int voices);
static void alsa_exit(int input);
static int alsa_mixer_volume(int volume);
static int alsa_buffer_size(void);



DIGI_DRIVER digi_alsa =
{
   DIGI_ALSA,
   empty_string,
   empty_string,
   "ALSA",
   0,
   0,
   MIXER_MAX_SFX,
   MIXER_DEF_SFX,

   alsa_detect,
   alsa_init,
   alsa_exit,
   alsa_mixer_volume,

   NULL,
   NULL,
   alsa_buffer_size,
   _mixer_init_voice,
   _mixer_release_voice,
   _mixer_start_voice,
   _mixer_stop_voice,
   _mixer_loop_voice,

   _mixer_get_position,
   _mixer_set_position,

   _mixer_get_volume,
   _mixer_set_volume,
   _mixer_ramp_volume,
   _mixer_stop_volume_ramp,

   _mixer_get_frequency,
   _mixer_set_frequency,
   _mixer_sweep_frequency,
   _mixer_stop_frequency_sweep,

   _mixer_get_pan,
   _mixer_set_pan,
   _mixer_sweep_pan,
   _mixer_stop_pan_sweep,

   _mixer_set_echo,
   _mixer_set_tremolo,
   _mixer_set_vibrato,
   0, 0,
   0,
   0,
   0,
   0,
   0,
   0
};



/* alsa_buffer_size:
 *  Returns the current DMA buffer size, for use by the audiostream code.
 */
static int alsa_buffer_size()
{
   return alsa_bufsize * alsa_fragments / (alsa_bits / 8) / (alsa_stereo ? 2 : 1);
}


#if ALLEGRO_ALSA_VERSION == 9

/* xrun_recovery:
 *  Underrun and suspend recovery
 */
static int xrun_recovery(snd_pcm_t *handle, int err)
{
   if (err == -EPIPE) {  /* under-run */
      err = snd_pcm_prepare(pcm_handle);
      if (err < 0)
	 fprintf(stderr, "Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
      return 0;
   }
   else if (err == -ESTRPIPE) {
      while ((err = snd_pcm_resume(pcm_handle)) == -EAGAIN)
	 sleep(1);  /* wait until the suspend flag is released */

      if (err < 0) {
	 err = snd_pcm_prepare(pcm_handle);
	 if (err < 0)
	   fprintf(stderr, "Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
      }
      return 0;
   }

   return err;
}



/* alsa_update:
 *  Updates main buffer.
 */
static void alsa_update(int threaded)
{
   int i, ret = 0;

   for(i = 0; i < alsa_fragments; i++) {
      while ((ret = snd_pcm_writei(pcm_handle, alsa_bufdata, alsa_bufsize / (alsa_bits / 8) / (alsa_stereo ? 2 : 1))) < 0) {
	 if (ret == -EAGAIN)
	    continue;

	 if (ret < 0) {
	    if (xrun_recovery(pcm_handle, ret) < 0)
	       fprintf(stderr, "Write error: %s\n", snd_strerror(ret));
	    break;  /* skip */
         }
      }

      _mix_some_samples((unsigned long) alsa_bufdata, 0, alsa_signed);
   }
}



/* alsa_detect:
 *  Detects driver presence.
 */
static int alsa_detect(int input)
{
   int ret = FALSE;
   char tmp1[128], tmp2[128];

   alsa_device = get_config_string(uconvert_ascii("sound", tmp1),
				   uconvert_ascii("alsa_device", tmp2),
				   alsa_device);
	
   ret = snd_pcm_open(&pcm_handle, alsa_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
   if(ret < 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not open card/pcm device"));
      return FALSE;
   }

   snd_pcm_close(pcm_handle);
   pcm_handle = NULL;
   return TRUE;	
}



/* alsa_init:
 *  ALSA init routine.
 */
static int alsa_init(int input, int voices)
{
   int ret = 0;
   char tmp1[128], tmp2[128];
   unsigned int dir=0;
   int format=0, bps=0, fragsize=0, numfrags=0;

   if (input) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Input is not supported"));
      return -1;
   }

   ALSA9_CHECK(snd_output_stdio_attach(&snd_output, stdout, 0));

   alsa_device = get_config_string(uconvert_ascii("sound", tmp1),
				   uconvert_ascii("alsa_device", tmp2),
				   alsa_device);

   fragsize = get_config_int(uconvert_ascii("sound", tmp1),
			     uconvert_ascii("alsa_fragsize", tmp2),
			     -1);

   numfrags = get_config_int(uconvert_ascii("sound", tmp1),
			     uconvert_ascii("alsa_numfrags", tmp2),
			     ALSA_DEFAULT_NUMFRAGS);
	
   ret = snd_pcm_open(&pcm_handle, alsa_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
   if(ret < 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not open card/pcm device"));
      return -1;
   }

   snd_mixer_open(&alsa_mixer, 0);

   if (alsa_mixer
       && snd_mixer_attach(alsa_mixer, alsa_device) >= 0
       && snd_mixer_selem_register (alsa_mixer, NULL, NULL) >= 0
       && snd_mixer_load(alsa_mixer) >= 0) {
      const char *alsa_mixer_elem_name = get_config_string(uconvert_ascii("sound", tmp1),
							   uconvert_ascii("alsa_mixer_elem", tmp2),
							   "PCM");

      alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);

      while (alsa_mixer_elem) {
	 const char *name = snd_mixer_selem_get_name(alsa_mixer_elem);

	 if (strcasecmp(name, alsa_mixer_elem_name) == 0) {
	    snd_mixer_selem_get_playback_volume_range(alsa_mixer_elem, &alsa_mixer_elem_min, &alsa_mixer_elem_max);
	    alsa_mixer_allegro_ratio = (double) (alsa_mixer_elem_max - alsa_mixer_elem_min) / (double) 255;
	    break;
	 }

	 alsa_mixer_elem = snd_mixer_elem_next(alsa_mixer_elem);
      }
   }

   /* Set format variables. */
   alsa_bits = (_sound_bits == 8) ? 8 : 16;
   alsa_stereo = (_sound_stereo) ? 1 : 0;
   alsa_rate = (_sound_freq > 0) ? _sound_freq : 44100;

   format = ((alsa_bits == 16) ? SND_PCM_FORMAT_S16_NE : SND_PCM_FORMAT_U8);

   alsa_signed = 0;
   bps = alsa_rate * (alsa_stereo ? 2 : 1);

   switch (format) {

      case SND_PCM_FORMAT_U8:
	 alsa_bits = 8;
	 break;

      case SND_PCM_FORMAT_S16_LE:
	 alsa_signed = 1;
	 bps <<= 1;
	 if (sizeof(short) != 2) {
	    ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Unsupported sample format"));
	    goto Error;
	 }
	 break;

      default:
	 ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Unsupported sample format"));
	 goto Error;
   }

   if (fragsize < 0) {
      bps >>= 9;
      if (bps < 16)
	 bps = 16;

      fragsize = 1;
      while ((fragsize << 1) < bps)
	 fragsize <<= 1;
   }
   else {
      fragsize = fragsize * (alsa_bits / 8) * (alsa_stereo ? 2 : 1);
   }

   snd_pcm_hw_params_malloc(&hwparams);
   snd_pcm_sw_params_malloc(&swparams);

   ALSA9_CHECK(snd_pcm_hw_params_any(pcm_handle, hwparams));
   ALSA9_CHECK(snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED));
   ALSA9_CHECK(snd_pcm_hw_params_set_format(pcm_handle, hwparams, format));
   ALSA9_CHECK(snd_pcm_hw_params_set_channels(pcm_handle, hwparams, alsa_stereo+1));

   snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &alsa_rate, &dir);

   ALSA9_CHECK(snd_pcm_hw_params_set_period_size(pcm_handle, hwparams, fragsize, dir));
   ALSA9_CHECK(snd_pcm_hw_params_set_periods(pcm_handle, hwparams, numfrags, dir));

   ALSA9_CHECK(snd_pcm_hw_params(pcm_handle, hwparams));

   ALSA9_CHECK(snd_pcm_hw_params_get_period_size(hwparams, &alsa_bufsize, &dir));
   ALSA9_CHECK(snd_pcm_hw_params_get_periods(hwparams, &alsa_fragments, &dir));

   /* Allocate mixing buffer. */
   alsa_bufdata = malloc(alsa_bufsize);
   if (!alsa_bufdata) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not allocate audio buffer"));
      goto Error;
   }

   /* Initialise mixer. */
   digi_alsa.voices = voices;

   if (_mixer_init(alsa_bufsize / (alsa_bits / 8), alsa_rate,
       alsa_stereo, ((alsa_bits == 16) ? 1 : 0), &digi_alsa.voices) != 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not init software mixer"));
      goto Error;
   }

   snd_pcm_prepare(pcm_handle);

   _mix_some_samples((unsigned long) alsa_bufdata, 0, alsa_signed);

   /* Add audio interrupt. */
   _unix_bg_man->register_func(alsa_update);

   uszprintf(alsa_desc, sizeof(alsa_desc),
	     get_config_text("Device '%s': %d bits, %s, %d bps, %s"),
	     alsa_device,
	     alsa_bits,
	     uconvert_ascii((alsa_signed ? "signed" : "unsigned"), tmp1),
	     alsa_rate,
	     uconvert_ascii((alsa_stereo ? "stereo" : "mono"), tmp2));

   digi_driver->desc = alsa_desc;
   return 0;

  Error:
   if (pcm_handle) {
      snd_pcm_close(pcm_handle);
      pcm_handle = NULL;
   }

   return -1;
}



/* alsa_exit:
 *  Shuts down ALSA driver.
 */
static void alsa_exit(int input)
{
   if (input)
      return;

   _unix_bg_man->unregister_func(alsa_update);

   free(alsa_bufdata);
   alsa_bufdata = NULL;

   _mixer_exit();

   if(alsa_mixer)
      snd_mixer_close(alsa_mixer);

   snd_pcm_close(pcm_handle);

   snd_pcm_hw_params_free(hwparams);
   snd_pcm_sw_params_free(swparams);
}


#else  /* ALLEGRO_ALSA_VERSION == 5 */


/* alsa_update:
 *  Update data.
 */
static void alsa_update(int threaded)
{
   int i;

   for (i = 0;  i < alsa_fragments; i++) {
      if (snd_pcm_write(pcm_handle, alsa_bufdata, alsa_bufsize) != alsa_bufsize)
	 break;
      _mix_some_samples((unsigned long) alsa_bufdata, 0, alsa_signed);
   }
}



/* alsa_detect:
 *  Detect driver presence.
 */
static int alsa_detect(int input)
{
   snd_pcm_t *handle;
   snd_pcm_info_t info;
   int card, device;
   char tmp1[128], tmp2[128];
   int ret = FALSE;

   card = get_config_int(uconvert_ascii("sound", tmp1),
			 uconvert_ascii("alsa_card", tmp2),
			 snd_defaults_card());

   device = get_config_int(uconvert_ascii("sound", tmp1),
			   uconvert_ascii("alsa_pcmdevice", tmp2),
			   snd_defaults_pcm_device());

   if (snd_pcm_open(&handle, card, device, (SND_PCM_OPEN_PLAYBACK
					    | SND_PCM_OPEN_NONBLOCK)) == 0) {
      if ((snd_pcm_info(handle, &info) == 0)
	  && (info.flags & SND_PCM_INFO_PLAYBACK))
	 ret = TRUE;
      
      snd_pcm_close(handle);
   }

   return ret;
}



/* alsa_init:
 *  ALSA init routine.
 */
static int alsa_init(int input, int voices)
{
   int card, device;
   int format, bps, fragsize, numfrags;
   snd_pcm_channel_params_t params;
   snd_pcm_channel_setup_t setup;
   char tmp1[128], tmp2[128];

   if (input) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Input is not supported"));
      return -1;
   }

   /* Load config.  */
   card = get_config_int(uconvert_ascii("sound", tmp1),
			 uconvert_ascii("alsa_card", tmp2),
			 snd_defaults_card());

   device = get_config_int(uconvert_ascii("sound", tmp1),
			   uconvert_ascii("alsa_pcmdevice", tmp2),
			   snd_defaults_pcm_device());

   fragsize = get_config_int(uconvert_ascii("sound", tmp1),
			     uconvert_ascii("alsa_fragsize", tmp2),
			     -1);

   numfrags = get_config_int(uconvert_ascii("sound", tmp1),
			     uconvert_ascii("alsa_numfrags", tmp2),
			     ALSA_DEFAULT_NUMFRAGS);

   /* Open PCM device.  */
   if (snd_pcm_open(&pcm_handle, card, device, (SND_PCM_OPEN_PLAYBACK
						| SND_PCM_OPEN_NONBLOCK)) < 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not open card/pcm device"));
      goto error;
   }

   /* Set format variables.  */
   alsa_bits = (_sound_bits == 8) ? 8 : 16;
   alsa_stereo = (_sound_stereo) ? 1 : 0;
   alsa_rate = (_sound_freq > 0) ? _sound_freq : 44100;

   format = ((alsa_bits == 16) ? SND_PCM_SFMT_S16_NE : SND_PCM_SFMT_U8);

   alsa_signed = 0;
   bps = alsa_rate * (alsa_stereo ? 2 : 1);
   switch (format) {
      case SND_PCM_SFMT_S8:
	 alsa_signed = 1;
      case SND_PCM_SFMT_U8:
	 alsa_bits = 8;
	 break;
      case SND_PCM_SFMT_S16_NE:
	 alsa_signed = 1;
      case SND_PCM_SFMT_U16_NE:
	 alsa_bits = 16;
	 bps <<= 1;
	 if (sizeof(short) != 2) {
	    ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Unsupported sample format"));
	    goto error;
	 }
	 break;
      default:
	 ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Unsupported sample format"));
	 goto error;
   }

   if (fragsize < 0) {
      bps >>= 9;
      if (bps < 16)
	 bps = 16;
      fragsize = 1;
      while ((fragsize << 1) < bps)
	 fragsize <<= 1;
   }
   else {
      fragsize = fragsize * (alsa_bits / 8) * (alsa_stereo ? 2 : 1);
   }
   
   /* Set PCM channel format.  */
   memset(&params, 0, sizeof(params));
   params.mode = SND_PCM_MODE_BLOCK;
   params.channel = SND_PCM_CHANNEL_PLAYBACK;
   params.start_mode = SND_PCM_START_FULL;
   params.stop_mode = SND_PCM_STOP_ROLLOVER;
   params.format.interleave = 1;
   params.format.format = format;
   params.format.rate = alsa_rate;
   params.format.voices = alsa_stereo + 1;
   params.buf.block.frag_size = fragsize;
   params.buf.block.frags_min = 1;
   params.buf.block.frags_max = numfrags;
   
   if (snd_pcm_channel_params(pcm_handle, &params) < 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not set channel parameters"));
      goto error;
   }

   snd_pcm_channel_prepare(pcm_handle, SND_PCM_CHANNEL_PLAYBACK);

   /* Read back fragments information.  */
   memset(&setup, 0, sizeof(setup));
   setup.mode = SND_PCM_MODE_BLOCK;
   setup.channel = SND_PCM_CHANNEL_PLAYBACK;

   if (snd_pcm_channel_setup(pcm_handle, &setup) < 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not get channel setup"));
      goto error;
   }

   alsa_fragments = numfrags;
   alsa_bufsize = setup.buf.block.frag_size;

   /* Allocate mixing buffer.  */
   alsa_bufdata = malloc(alsa_bufsize);
   if (!alsa_bufdata) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not allocate audio buffer"));
      goto error;
   }

   /* Initialise mixer.  */
   digi_alsa.voices = voices;

   if (_mixer_init(alsa_bufsize / (alsa_bits / 8), alsa_rate,
		   alsa_stereo, ((alsa_bits == 16) ? 1 : 0),
		   &digi_alsa.voices) != 0) {
      ustrzcpy(allegro_error, ALLEGRO_ERROR_SIZE, get_config_text("Can not init software mixer"));
      goto error;
   }

   _mix_some_samples((unsigned long) alsa_bufdata, 0, alsa_signed);

   /* Add audio interrupt.  */
   _unix_bg_man->register_func(alsa_update);

   uszprintf(alsa_desc, sizeof(alsa_desc),
	    get_config_text("Card #%d, device #%d: %d bits, %s, %d bps, %s"),
	    card, device, alsa_bits,
	    uconvert_ascii((alsa_signed ? "signed" : "unsigned"), tmp1),
	    alsa_rate,
	    uconvert_ascii((alsa_stereo ? "stereo" : "mono"), tmp2));

   digi_driver->desc = alsa_desc;

   return 0;

  error:

   if (pcm_handle) {
      snd_pcm_close(pcm_handle);
      pcm_handle = NULL;
   }
   
   return -1;
}



/* alsa_exit:
 *  Shutdown ALSA driver.
 */
static void alsa_exit(int input)
{
   if (input) {
      return;
   }

   _unix_bg_man->unregister_func(alsa_update);

   free(alsa_bufdata);
   alsa_bufdata = NULL;

   _mixer_exit();

   snd_pcm_close(pcm_handle);
}

#endif


/* alsa_mixer_volume:
 *  Set mixer volume (0-255)
 */
static int alsa_mixer_volume(int volume)
{
#if ALLEGRO_ALSA_VERSION == 9
   unsigned long left_vol = 0, right_vol = 0;
 
   if (alsa_mixer && alsa_mixer_elem) {
      snd_mixer_selem_get_playback_volume(alsa_mixer_elem, 0, &left_vol);
      snd_mixer_selem_get_playback_volume(alsa_mixer_elem, 1, &right_vol);
      snd_mixer_selem_set_playback_volume(alsa_mixer_elem, 0, (int)floor(left_vol+0.5) * alsa_mixer_allegro_ratio);
      snd_mixer_selem_set_playback_volume(alsa_mixer_elem, 1, (int)floor(right_vol+0.5) * alsa_mixer_allegro_ratio);
   }
#else  /* ALLEGRO_ALSA_VERSION == 5 */
   /* TODO */ 
#if 0
   snd_mixer_t *handle;
   int card, device;

   if (snd_mixer_open(&handle, card, device) == 0) {
      /* do something special */
      snd_mixer_close(handle);
      return 0;
   }

   return -1;
#endif
#endif
   return 0;
}



#ifdef ALLEGRO_MODULE

/* _module_init:
 *  Called when loaded as a dynamically linked module.
 */
void _module_init(int system_driver)
{
   _unix_register_digi_driver(DIGI_ALSA, &digi_alsa, TRUE, TRUE);
}

#endif

#endif

