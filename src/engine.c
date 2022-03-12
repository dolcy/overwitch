/*
 * engine.c
 * Copyright (C) 2019 Stefan Rehm <droelfdroelf@gmail.com>
 * Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <endian.h>
#include <time.h>
#include <unistd.h>
#include "dll.h"
#include "engine.h"

#define AUDIO_IN_EP  0x83
#define AUDIO_OUT_EP 0x03
#define MIDI_IN_EP   0x81
#define MIDI_OUT_EP  0x01

#define MIDI_BUF_EVENTS 64
#define MIDI_BUF_SIZE (MIDI_BUF_EVENTS * OB_MIDI_EVENT_SIZE)

#define USB_BULK_MIDI_SIZE 512

#define SAMPLE_TIME_NS (1e9 / ((int)OB_SAMPLE_RATE))

struct ow_engine_usb_blk
{
  uint16_t header;
  uint16_t frames;
  uint8_t padding[OB_PADDING_SIZE];
  int32_t data[];
};

static void prepare_cycle_in ();
static void prepare_cycle_out ();
static void prepare_cycle_in_midi ();

static struct ow_engine_usb_blk *
get_nth_usb_in_blk (struct ow_engine *engine, int n)
{
  char *blk = &engine->usb_data_in[n * engine->usb_data_in_blk_len];
  return (struct ow_engine_usb_blk *) blk;
}

static struct ow_engine_usb_blk *
get_nth_usb_out_blk (struct ow_engine *engine, int n)
{
  char *blk = &engine->usb_data_out[n * engine->usb_data_out_blk_len];
  return (struct ow_engine_usb_blk *) blk;
}

static int
prepare_transfers (struct ow_engine *engine)
{
  engine->xfr_in = libusb_alloc_transfer (0);
  if (!engine->xfr_in)
    {
      return -ENOMEM;
    }

  engine->xfr_out = libusb_alloc_transfer (0);
  if (!engine->xfr_out)
    {
      return -ENOMEM;
    }

  engine->xfr_in_midi = libusb_alloc_transfer (0);
  if (!engine->xfr_in_midi)
    {
      return -ENOMEM;
    }

  engine->xfr_out_midi = libusb_alloc_transfer (0);
  if (!engine->xfr_out_midi)
    {
      return -ENOMEM;
    }

  return LIBUSB_SUCCESS;
}

static void
free_transfers (struct ow_engine *engine)
{
  libusb_free_transfer (engine->xfr_in);
  libusb_free_transfer (engine->xfr_out);
  libusb_free_transfer (engine->xfr_in_midi);
  libusb_free_transfer (engine->xfr_out_midi);
}

static void
set_usb_input_data_blks (struct ow_engine *engine)
{
  struct ow_engine_usb_blk *blk;
  size_t wso2j;
  int32_t hv;
  float *f;
  int32_t *s;
  ow_engine_status_t status;

  pthread_spin_lock (&engine->lock);
  if (engine->dll_ow)
    {
      ow_dll_overwitch_inc (engine->dll_ow, engine->frames_per_transfer,
			    engine->io_buffers->get_time ());
    }
  status = engine->status;
  pthread_spin_unlock (&engine->lock);

  f = engine->o2p_transfer_buf;
  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = get_nth_usb_in_blk (engine, i);
      s = blk->data;
      for (int j = 0; j < OB_FRAMES_PER_BLOCK; j++)
	{
	  for (int k = 0; k < engine->device_desc->outputs; k++)
	    {
	      hv = be32toh (*s);
	      *f = hv / (float) INT_MAX;
	      f++;
	      s++;
	    }
	}
    }

  if (status < OW_STATUS_RUN)
    {
      return;
    }

  wso2j = engine->io_buffers->write_space (engine->io_buffers->o2p_audio);
  if (engine->o2p_transfer_size <= wso2j)
    {
      engine->io_buffers->write (engine->io_buffers->o2p_audio,
				 (void *) engine->o2p_transfer_buf,
				 engine->o2p_transfer_size);
    }
  else
    {
      error_print ("o2j: Audio ring buffer overflow. Discarding data...\n");
    }
}

static void
set_usb_output_data_blks (struct ow_engine *engine)
{
  struct ow_engine_usb_blk *blk;
  size_t rsj2o;
  int32_t hv;
  size_t bytes;
  long frames;
  float *f;
  int res;
  int32_t *s;
  int enabled = ow_engine_is_p2o_audio_enable (engine);

  rsj2o = engine->io_buffers->read_space (engine->io_buffers->p2o_audio);
  if (!engine->reading_at_p2o_end)
    {
      if (enabled && rsj2o >= engine->p2o_transfer_size)
	{
	  debug_print (2, "j2o: Emptying buffer and running...\n");
	  bytes = ow_bytes_to_frame_bytes (rsj2o, engine->p2o_frame_size);
	  engine->io_buffers->read (engine->io_buffers->p2o_audio, NULL,
				    bytes);
	  engine->reading_at_p2o_end = 1;
	}
      goto set_blocks;
    }

  if (!enabled)
    {
      engine->reading_at_p2o_end = 0;
      debug_print (2, "j2o: Clearing buffer and stopping...\n");
      memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
      goto set_blocks;
    }

  pthread_spin_lock (&engine->lock);
  engine->p2o_latency = rsj2o;
  if (engine->p2o_latency > engine->p2o_max_latency)
    {
      engine->p2o_max_latency = engine->p2o_latency;
    }
  pthread_spin_unlock (&engine->lock);

  if (rsj2o >= engine->p2o_transfer_size)
    {
      engine->io_buffers->read (engine->io_buffers->p2o_audio,
				(void *) engine->p2o_transfer_buf,
				engine->p2o_transfer_size);
    }
  else
    {
      debug_print (2,
		   "j2o: Audio ring buffer underflow (%zu < %zu). Resampling...\n",
		   rsj2o, engine->p2o_transfer_size);
      frames = rsj2o / engine->p2o_frame_size;
      bytes = frames * engine->p2o_frame_size;
      engine->io_buffers->read (engine->io_buffers->p2o_audio,
				(void *) engine->p2o_resampler_buf, bytes);
      engine->p2o_data.input_frames = frames;
      engine->p2o_data.src_ratio =
	(double) engine->frames_per_transfer / frames;
      //We should NOT use the simple API but since this only happens very occasionally and mostly at startup, this has very low impact on audio quality.
      res = src_simple (&engine->p2o_data, SRC_SINC_FASTEST,
			engine->device_desc->inputs);
      if (res)
	{
	  debug_print (2, "j2o: Error while resampling: %s\n",
		       src_strerror (res));
	}
      else if (engine->p2o_data.output_frames_gen !=
	       engine->frames_per_transfer)
	{
	  error_print
	    ("j2o: Unexpected frames with ratio %f (output %ld, expected %d)\n",
	     engine->p2o_data.src_ratio, engine->p2o_data.output_frames_gen,
	     engine->frames_per_transfer);
	}
    }

set_blocks:
  f = engine->p2o_transfer_buf;
  for (int i = 0; i < engine->blocks_per_transfer; i++)
    {
      blk = get_nth_usb_out_blk (engine, i);
      engine->frames += OB_FRAMES_PER_BLOCK;
      blk->frames = htobe16 (engine->frames);
      s = blk->data;
      for (int j = 0; j < OB_FRAMES_PER_BLOCK; j++)
	{
	  for (int k = 0; k < engine->device_desc->inputs; k++)
	    {
	      hv = htobe32 ((int32_t) (*f * INT_MAX));
	      *s = hv;
	      f++;
	      s++;
	    }
	}
    }
}

static void LIBUSB_CALL
cb_xfr_in (struct libusb_transfer *xfr)
{
  if (xfr->status == LIBUSB_TRANSFER_COMPLETED)
    {
      set_usb_input_data_blks (xfr->user_data);
    }
  else
    {
      error_print ("o2j: Error on USB audio transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
  // start new cycle even if this one did not succeed
  prepare_cycle_in (xfr->user_data);
}

static void LIBUSB_CALL
cb_xfr_out (struct libusb_transfer *xfr)
{
  if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
    {
      error_print ("j2o: Error on USB audio transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
  set_usb_output_data_blks (xfr->user_data);
  // We have to make sure that the out cycle is always started after its callback
  // Race condition on slower systems!
  prepare_cycle_out (xfr->user_data);
}

static void LIBUSB_CALL
cb_xfr_in_midi (struct libusb_transfer *xfr)
{
  struct ow_midi_event event;
  int length;
  struct ow_engine *engine = xfr->user_data;

  if (ow_engine_get_status (engine) < OW_STATUS_RUN)
    {
      goto end;
    }

  if (xfr->status == LIBUSB_TRANSFER_COMPLETED)
    {
      length = 0;
      event.time = engine->io_buffers->get_time ();

      while (length < xfr->actual_length)
	{
	  memcpy (event.bytes, &engine->o2p_midi_data[length],
		  OB_MIDI_EVENT_SIZE);
	  //Note-off, Note-on, Poly-KeyPress, Control Change, Program Change, Channel Pressure, PitchBend Change, Single Byte
	  if (event.bytes[0] >= 0x08 && event.bytes[0] <= 0x0f)
	    {
	      debug_print (2, "o2j MIDI: %02x, %02x, %02x, %02x (%f)\n",
			   event.bytes[0], event.bytes[1], event.bytes[2],
			   event.bytes[3], event.time);

	      if (engine->io_buffers->
		  write_space (engine->io_buffers->o2p_midi) >=
		  sizeof (struct ow_midi_event))
		{
		  engine->io_buffers->write (engine->io_buffers->o2p_midi,
					     (void *) &event,
					     sizeof (struct ow_midi_event));
		}
	      else
		{
		  error_print
		    ("o2j: MIDI ring buffer overflow. Discarding data...\n");
		}
	    }
	  length += OB_MIDI_EVENT_SIZE;
	}
    }
  else
    {
      if (xfr->status != LIBUSB_TRANSFER_TIMED_OUT)
	{
	  error_print ("Error on USB MIDI in transfer: %s\n",
		       libusb_strerror (xfr->status));
	}
    }

end:
  prepare_cycle_in_midi (engine);
}

static void LIBUSB_CALL
cb_xfr_out_midi (struct libusb_transfer *xfr)
{
  struct ow_engine *engine = xfr->user_data;

  pthread_spin_lock (&engine->p2o_midi_lock);
  engine->p2o_midi_ready = 1;
  pthread_spin_unlock (&engine->p2o_midi_lock);

  if (xfr->status != LIBUSB_TRANSFER_COMPLETED)
    {
      error_print ("Error on USB MIDI out transfer: %s\n",
		   libusb_strerror (xfr->status));
    }
}

static void
prepare_cycle_out (struct ow_engine *engine)
{
  libusb_fill_interrupt_transfer (engine->xfr_out, engine->device_handle,
				  AUDIO_OUT_EP, (void *) engine->usb_data_out,
				  engine->usb_data_out_len, cb_xfr_out,
				  engine, 0);

  int err = libusb_submit_transfer (engine->xfr_out);
  if (err)
    {
      error_print ("j2o: Error when submitting USB audio transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_STATUS_ERROR);
    }
}

static void
prepare_cycle_in (struct ow_engine *engine)
{
  libusb_fill_interrupt_transfer (engine->xfr_in, engine->device_handle,
				  AUDIO_IN_EP, (void *) engine->usb_data_in,
				  engine->usb_data_in_len, cb_xfr_in, engine,
				  0);

  int err = libusb_submit_transfer (engine->xfr_in);
  if (err)
    {
      error_print ("o2j: Error when submitting USB audio in transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_STATUS_ERROR);
    }
}

static void
prepare_cycle_in_midi (struct ow_engine *engine)
{
  libusb_fill_bulk_transfer (engine->xfr_in_midi, engine->device_handle,
			     MIDI_IN_EP, (void *) engine->o2p_midi_data,
			     USB_BULK_MIDI_SIZE, cb_xfr_in_midi, engine, 0);

  int err = libusb_submit_transfer (engine->xfr_in_midi);
  if (err)
    {
      error_print ("o2j: Error when submitting USB MIDI transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_STATUS_ERROR);
    }
}

static void
prepare_cycle_out_midi (struct ow_engine *engine)
{
  libusb_fill_bulk_transfer (engine->xfr_out_midi, engine->device_handle,
			     MIDI_OUT_EP, (void *) engine->p2o_midi_data,
			     USB_BULK_MIDI_SIZE, cb_xfr_out_midi, engine, 0);

  int err = libusb_submit_transfer (engine->xfr_out_midi);
  if (err)
    {
      error_print ("j2o: Error when submitting USB MIDI transfer: %s\n",
		   libusb_strerror (err));
      ow_engine_set_status (engine, OW_STATUS_ERROR);
    }
}

static void
usb_shutdown (struct ow_engine *engine)
{
  libusb_close (engine->device_handle);
  libusb_exit (engine->context);
}

// initialization taken from sniffed session

ow_err_t
ow_engine_init (struct ow_engine **engine_,
		uint8_t bus, uint8_t address, int blocks_per_transfer)
{
  int i, err;
  ow_err_t ret = OW_OK;
  libusb_device **list = NULL;
  ssize_t count = 0;
  libusb_device *device;
  char *name = NULL;
  struct libusb_device_descriptor desc;
  struct ow_engine_usb_blk *blk;
  struct ow_engine *engine = malloc (sizeof (struct ow_engine));

  // libusb setup
  if (libusb_init (&engine->context) != LIBUSB_SUCCESS)
    {
      ret = OW_USB_ERROR_LIBUSB_INIT_FAILED;
      goto end;
    }

  engine->device_handle = NULL;
  count = libusb_get_device_list (engine->context, &list);
  for (i = 0; i < count; i++)
    {
      device = list[i];
      err = libusb_get_device_descriptor (device, &desc);
      if (err)
	{
	  error_print ("Error while getting device description: %s",
		       libusb_error_name (err));
	  continue;
	}

      if (ow_is_valid_device (desc.idVendor, desc.idProduct, &name) &&
	  libusb_get_bus_number (device) == bus &&
	  libusb_get_device_address (device) == address)
	{
	  if (libusb_open (device, &engine->device_handle))
	    {
	      error_print ("Error while opening device: %s\n",
			   libusb_error_name (err));
	    }
	  break;
	}
    }

  err = 0;
  libusb_free_device_list (list, count);

  if (!engine->device_handle)
    {
      ret = OW_USB_ERROR_CANT_FIND_DEV;
      goto end;
    }

  for (const struct ow_device_desc ** d = OB_DEVICE_DESCS; *d != NULL; d++)
    {
      debug_print (2, "Checking for %s...\n", (*d)->name);
      if (strcmp ((*d)->name, name) == 0)
	{
	  engine->device_desc = *d;
	  break;
	}
    }

  if (!engine->device_desc)
    {
      ret = OW_USB_ERROR_CANT_FIND_DEV;
      goto end;
    }

  printf ("Device: %s (outputs: %d, inputs: %d)\n", engine->device_desc->name,
	  engine->device_desc->outputs, engine->device_desc->inputs);

  err = libusb_set_configuration (engine->device_handle, 1);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_USB_CONFIG;
      goto end;
    }
  err = libusb_claim_interface (engine->device_handle, 1);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->device_handle, 1, 3);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_claim_interface (engine->device_handle, 2);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->device_handle, 2, 2);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_claim_interface (engine->device_handle, 3);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLAIM_IF;
      goto end;
    }
  err = libusb_set_interface_alt_setting (engine->device_handle, 3, 0);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_SET_ALT_SETTING;
      goto end;
    }
  err = libusb_clear_halt (engine->device_handle, AUDIO_IN_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->device_handle, AUDIO_OUT_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->device_handle, MIDI_IN_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = libusb_clear_halt (engine->device_handle, MIDI_OUT_EP);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_CLEAR_EP;
      goto end;
    }
  err = prepare_transfers (engine);
  if (LIBUSB_SUCCESS != err)
    {
      ret = OW_USB_ERROR_CANT_PREPARE_TRANSFER;
    }

end:
  if (ret == OW_OK)
    {
      pthread_spin_init (&engine->lock, PTHREAD_PROCESS_SHARED);

      engine->blocks_per_transfer = blocks_per_transfer;
      engine->frames_per_transfer =
	OB_FRAMES_PER_BLOCK * engine->blocks_per_transfer;

      engine->p2o_audio_enabled = 0;

      engine->usb_data_in_blk_len =
	sizeof (struct ow_engine_usb_blk) +
	sizeof (int32_t) * OB_FRAMES_PER_BLOCK * engine->device_desc->outputs;
      engine->usb_data_out_blk_len =
	sizeof (struct ow_engine_usb_blk) +
	sizeof (int32_t) * OB_FRAMES_PER_BLOCK * engine->device_desc->inputs;

      engine->usb_data_in_len =
	engine->usb_data_in_blk_len * engine->blocks_per_transfer;
      engine->usb_data_out_len =
	engine->usb_data_out_blk_len * engine->blocks_per_transfer;
      engine->usb_data_in = malloc (engine->usb_data_in_len);
      engine->usb_data_out = malloc (engine->usb_data_out_len);
      memset (engine->usb_data_in, 0, engine->usb_data_in_len);
      memset (engine->usb_data_out, 0, engine->usb_data_out_len);

      for (int i = 0; i < engine->blocks_per_transfer; i++)
	{
	  blk = get_nth_usb_out_blk (engine, i);
	  blk->header = htobe16 (0x07ff);
	}

      engine->p2o_frame_size =
	OB_BYTES_PER_SAMPLE * engine->device_desc->inputs;
      engine->o2p_frame_size =
	OB_BYTES_PER_SAMPLE * engine->device_desc->outputs;

      engine->p2o_transfer_size =
	engine->frames_per_transfer * engine->p2o_frame_size;
      engine->o2p_transfer_size =
	engine->frames_per_transfer * engine->o2p_frame_size;
      engine->p2o_transfer_buf = malloc (engine->p2o_transfer_size);
      engine->o2p_transfer_buf = malloc (engine->o2p_transfer_size);
      memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
      memset (engine->o2p_transfer_buf, 0, engine->o2p_transfer_size);

      //o2j resampler
      engine->p2o_resampler_buf = malloc (engine->p2o_transfer_size);
      memset (engine->p2o_resampler_buf, 0, engine->p2o_transfer_size);
      engine->p2o_data.data_in = engine->p2o_resampler_buf;
      engine->p2o_data.data_out = engine->p2o_transfer_buf;
      engine->p2o_data.end_of_input = 1;
      engine->p2o_data.input_frames = engine->frames_per_transfer;
      engine->p2o_data.output_frames = engine->frames_per_transfer;

      //MIDI
      engine->p2o_midi_data = malloc (USB_BULK_MIDI_SIZE);
      engine->o2p_midi_data = malloc (USB_BULK_MIDI_SIZE);
      memset (engine->p2o_midi_data, 0, USB_BULK_MIDI_SIZE);
      memset (engine->o2p_midi_data, 0, USB_BULK_MIDI_SIZE);
      pthread_spin_init (&engine->p2o_midi_lock, PTHREAD_PROCESS_SHARED);

      *engine_ = engine;
    }
  else
    {
      usb_shutdown (engine);
      free (engine);
      error_print ("Error while initializing device: %s\n",
		   libusb_error_name (ret));
    }
  return ret;
}

static const char *ob_err_strgs[] = {
  "ok",
  "libusb init failed",
  "can't open device",
  "can't set usb config",
  "can't claim usb interface",
  "can't set usb alt setting",
  "can't cleat endpoint",
  "can't prepare transfer",
  "can't find a matching device",
  "'buffer_read_space' not set",
  "'buffer_write_space' not set",
  "'buffer_read' not set",
  "'buffer_write' not set",
  "'get_time' not set",
  "'p2o_audio_buf' not set",
  "'o2p_audio_buf' not set",
  "'p2o_midi_buf' not set",
  "'o2p_midi_buf' not set"
};

static void *
run_p2o_midi (void *data)
{
  int pos, p2o_midi_ready, event_read = 0;
  double last_time, diff;
  struct timespec sleep_time, smallest_sleep_time;
  struct ow_midi_event event;
  struct ow_engine *engine = data;

  smallest_sleep_time.tv_sec = 0;
  smallest_sleep_time.tv_nsec = SAMPLE_TIME_NS * 32 / 2;	//Average wait time for a 32 buffer sample

  pos = 0;
  diff = 0.0;
  last_time = engine->io_buffers->get_time ();
  engine->p2o_midi_ready = 1;
  while (1)
    {

      while (engine->io_buffers->read_space (engine->io_buffers->p2o_midi) >=
	     sizeof (struct ow_midi_event) && pos < USB_BULK_MIDI_SIZE)
	{
	  if (!pos)
	    {
	      memset (engine->p2o_midi_data, 0, USB_BULK_MIDI_SIZE);
	      diff = 0;
	    }

	  if (!event_read)
	    {
	      engine->io_buffers->read (engine->io_buffers->p2o_midi,
					(void *) &event,
					sizeof (struct ow_midi_event));
	      event_read = 1;
	    }

	  if (event.time > last_time)
	    {
	      diff = event.time - last_time;
	      last_time = event.time;
	      break;
	    }

	  memcpy (&engine->p2o_midi_data[pos], event.bytes,
		  OB_MIDI_EVENT_SIZE);
	  pos += OB_MIDI_EVENT_SIZE;
	  event_read = 0;
	}

      if (pos)
	{
	  debug_print (2, "Event frames: %f; diff: %f\n", event.time, diff);
	  engine->p2o_midi_ready = 0;
	  prepare_cycle_out_midi (engine);
	  pos = 0;
	}

      if (diff)
	{
	  sleep_time.tv_sec = diff;
	  sleep_time.tv_nsec = (diff - sleep_time.tv_sec) * 1.0e9;
	  nanosleep (&sleep_time, NULL);
	}
      else
	{
	  nanosleep (&smallest_sleep_time, NULL);
	}

      pthread_spin_lock (&engine->p2o_midi_lock);
      p2o_midi_ready = engine->p2o_midi_ready;
      pthread_spin_unlock (&engine->p2o_midi_lock);
      while (!p2o_midi_ready)
	{
	  nanosleep (&smallest_sleep_time, NULL);
	  pthread_spin_lock (&engine->p2o_midi_lock);
	  p2o_midi_ready = engine->p2o_midi_ready;
	  pthread_spin_unlock (&engine->p2o_midi_lock);
	};

      if (ow_engine_get_status (engine) <= OW_STATUS_STOP)
	{
	  break;
	}
    }

  return NULL;
}

static void *
run_audio_o2p_midi (void *data)
{
  size_t rsj2o, bytes;
  struct ow_engine *engine = data;

  while (ow_engine_get_status (engine) == OW_STATUS_READY);

  //status == OW_STATUS_BOOT

  prepare_cycle_in (engine);
  prepare_cycle_out (engine);
  if (engine->midi)
    {
      prepare_cycle_in_midi (engine);
    }

  while (1)
    {
      engine->p2o_latency = 0;
      engine->p2o_max_latency = 0;
      engine->reading_at_p2o_end = 0;

      //status == OW_STATUS_BOOT

      pthread_spin_lock (&engine->lock);

      if (engine->dll_ow)
	{
	  ow_dll_overwitch_init (engine->dll_ow, OB_SAMPLE_RATE,
				 engine->frames_per_transfer,
				 engine->io_buffers->get_time ());
	}

      engine->status = OW_STATUS_WAIT;
      pthread_spin_unlock (&engine->lock);

      while (ow_engine_get_status (engine) >= OW_STATUS_WAIT)
	{
	  libusb_handle_events_completed (engine->context, NULL);
	}

      if (ow_engine_get_status (engine) <= OW_STATUS_STOP)
	{
	  break;
	}

      ow_engine_set_status (engine, OW_STATUS_BOOT);

      rsj2o = engine->io_buffers->read_space (engine->io_buffers->p2o_audio);
      bytes = ow_bytes_to_frame_bytes (rsj2o, engine->p2o_frame_size);
      engine->io_buffers->read (engine->io_buffers->p2o_audio, NULL, bytes);
      memset (engine->p2o_transfer_buf, 0, engine->p2o_transfer_size);
    }

  return NULL;
}

ow_err_t
ow_engine_activate_with_dll (struct ow_engine *engine,
			     struct ow_io_buffers *io_buffers,
			     struct ow_dll_overwitch *dll_ow)
{
  engine->io_buffers = io_buffers;
  engine->dll_ow = dll_ow;

  if (!io_buffers->read_space)
    {
      return OW_INIT_ERROR_NO_READ_SPACE;
    }
  if (!io_buffers->write_space)
    {
      return OW_INIT_ERROR_NO_WRITE_SPACE;
    }
  if (!io_buffers->read)
    {
      return OW_INIT_ERROR_NO_READ;
    }
  if (!io_buffers->write)
    {
      return OW_INIT_ERROR_NO_WRITE;
    }
  if (!io_buffers->o2p_audio)
    {
      return OW_INIT_ERROR_NO_O2P_AUDIO_BUF;
    }
  if (!io_buffers->p2o_audio)
    {
      return OW_INIT_ERROR_NO_P2O_AUDIO_BUF;
    }

  if (!io_buffers->get_time && !io_buffers->o2p_midi && !io_buffers->p2o_midi)
    {
      engine->midi = 0;
    }
  else
    {
      if (!io_buffers->get_time)
	{
	  return OW_INIT_ERROR_NO_GET_TIME;
	}
      if (!io_buffers->o2p_midi)
	{
	  return OW_INIT_ERROR_NO_O2P_MIDI_BUF;
	}
      if (!io_buffers->p2o_midi)
	{
	  return OW_INIT_ERROR_NO_P2O_MIDI_BUF;
	}
      engine->midi = 1;
    }

  if (dll_ow)
    {
      if (!io_buffers->get_time)
	{
	  return OW_INIT_ERROR_NO_GET_TIME;
	}
    }

  engine->frames = 0;

  engine->status = OW_STATUS_READY;
  if (engine->midi)
    {
      debug_print (1, "Starting j2o MIDI thread...\n");
      if (pthread_create (&engine->p2o_midi_thread, NULL, run_p2o_midi,
			  engine))
	{
	  error_print ("Could not start MIDI thread\n");
	  return OW_GENERIC_ERROR;
	}
    }

  debug_print (1, "Starting audio and o2j MIDI thread...\n");
  if (pthread_create (&engine->audio_o2p_midi_thread, NULL,
		      run_audio_o2p_midi, engine))
    {
      error_print ("Could not start device thread\n");
      return OW_GENERIC_ERROR;
    }

  return OW_OK;
}

ow_err_t
ow_engine_activate (struct ow_engine *engine,
		    struct ow_io_buffers *io_buffers)
{
  return ow_engine_activate_with_dll (engine, io_buffers, NULL);
}

inline void
ow_engine_wait (struct ow_engine *engine)
{
  pthread_join (engine->audio_o2p_midi_thread, NULL);
  if (engine->midi)
    {
      pthread_join (engine->p2o_midi_thread, NULL);
    }
}

const char *
ow_get_err_str (ow_err_t errcode)
{
  return ob_err_strgs[errcode];
}

void
ow_engine_destroy (struct ow_engine *engine)
{
  usb_shutdown (engine);
  free_transfers (engine);
  free (engine->p2o_transfer_buf);
  free (engine->p2o_resampler_buf);
  free (engine->o2p_transfer_buf);
  free (engine->usb_data_in);
  free (engine->usb_data_out);
  free (engine->p2o_midi_data);
  free (engine->o2p_midi_data);
  pthread_spin_destroy (&engine->lock);
  pthread_spin_destroy (&engine->p2o_midi_lock);
  free (engine);
}

inline ow_engine_status_t
ow_engine_get_status (struct ow_engine *engine)
{
  ow_engine_status_t status;
  pthread_spin_lock (&engine->lock);
  status = engine->status;
  pthread_spin_unlock (&engine->lock);
  return status;
}

inline void
ow_engine_set_status (struct ow_engine *engine, ow_engine_status_t status)
{
  pthread_spin_lock (&engine->lock);
  engine->status = status;
  pthread_spin_unlock (&engine->lock);
}

inline int
ow_engine_is_p2o_audio_enable (struct ow_engine *engine)
{
  int enabled;
  pthread_spin_lock (&engine->lock);
  enabled = engine->p2o_audio_enabled;
  pthread_spin_unlock (&engine->lock);
  return enabled;
}

inline void
ow_engine_set_p2o_audio_enable (struct ow_engine *engine, int enabled)
{
  int last = ow_engine_is_p2o_audio_enable (engine);
  if (last != enabled)
    {
      pthread_spin_lock (&engine->lock);
      engine->p2o_audio_enabled = enabled;
      pthread_spin_unlock (&engine->lock);
      debug_print (1, "Setting j2o audio to %d...\n", enabled);
    }
}

inline int
ow_bytes_to_frame_bytes (int bytes, int bytes_per_frame)
{
  int frames = bytes / bytes_per_frame;
  return frames * bytes_per_frame;
}

const struct ow_device_desc *
ow_engine_get_device_desc (struct ow_engine *engine)
{
  return engine->device_desc;
}

inline void
ow_engine_stop (struct ow_engine *engine)
{
  ow_engine_set_status (engine, OW_STATUS_STOP);
}