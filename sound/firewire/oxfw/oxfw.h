/*
 * oxfw.h - a part of driver for OXFW970/971 based devices
 *
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include <linux/device.h>
#include <linux/firewire.h>
#include <linux/firewire-constants.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>

/* TODO: remove when merging to upstream. */
#include "../../../backport.h"

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/info.h>
#include <sound/rawmidi.h>
#include <sound/hwdep.h>
//#include <sound/firewire.h>

#include "../lib.h"
#include "../fcp.h"
#include "../packets-buffer.h"
#include "../iso-resources.h"
#include "../amdtp-am824.h"
#include "../cmp.h"

/* This is an arbitrary number for convinience. */
#define	SND_OXFW_STREAM_FORMAT_ENTRIES	10
struct snd_oxfw {
	struct snd_card *card;
	struct fw_unit *unit;
	struct mutex mutex;
	spinlock_t lock;

	bool registered;
	struct delayed_work dwork;

	bool wrong_dbs;
	bool has_output;
	u8 *tx_stream_formats[SND_OXFW_STREAM_FORMAT_ENTRIES];
	u8 *rx_stream_formats[SND_OXFW_STREAM_FORMAT_ENTRIES];
	bool assumed;
	struct cmp_connection out_conn;
	struct cmp_connection in_conn;
	struct amdtp_stream tx_stream;
	struct amdtp_stream rx_stream;
	unsigned int capture_substreams;
	unsigned int playback_substreams;

	unsigned int midi_input_ports;
	unsigned int midi_output_ports;

	int dev_lock_count;
	bool dev_lock_changed;
	wait_queue_head_t hwdep_wait;

	const struct ieee1394_device_id *entry;
	void *spec;
};

/*
 * AV/C Digital Interface Command Set General Specification 4.2
 * (Sep 2004, 1394TA)
 */
int avc_general_inquiry_sig_fmt(struct fw_unit *unit, unsigned int rate,
				enum avc_general_plug_dir dir,
				unsigned short pid);

int snd_oxfw_stream_init_simplex(struct snd_oxfw *oxfw,
				 struct amdtp_stream *stream);
int snd_oxfw_stream_start_simplex(struct snd_oxfw *oxfw,
				  struct amdtp_stream *stream,
				  unsigned int rate, unsigned int pcm_channels);
void snd_oxfw_stream_stop_simplex(struct snd_oxfw *oxfw,
				  struct amdtp_stream *stream);
void snd_oxfw_stream_destroy_simplex(struct snd_oxfw *oxfw,
				     struct amdtp_stream *stream);
void snd_oxfw_stream_update_simplex(struct snd_oxfw *oxfw,
				    struct amdtp_stream *stream);

int snd_oxfw_stream_get_current_formation(struct snd_oxfw *oxfw,
				enum avc_general_plug_dir dir,
				struct avc_stream_formation *formation);

int snd_oxfw_stream_discover(struct snd_oxfw *oxfw);

void snd_oxfw_stream_lock_changed(struct snd_oxfw *oxfw);
int snd_oxfw_stream_lock_try(struct snd_oxfw *oxfw);
void snd_oxfw_stream_lock_release(struct snd_oxfw *oxfw);

int snd_oxfw_create_pcm(struct snd_oxfw *oxfw);

void snd_oxfw_proc_init(struct snd_oxfw *oxfw);

int snd_oxfw_create_midi(struct snd_oxfw *oxfw);

int snd_oxfw_create_hwdep(struct snd_oxfw *oxfw);

int snd_oxfw_add_spkr(struct snd_oxfw *oxfw, bool is_lacie);
int snd_oxfw_scs1x_add(struct snd_oxfw *oxfw);
void snd_oxfw_scs1x_update(struct snd_oxfw *oxfw);
