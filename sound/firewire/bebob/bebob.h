/*
 * bebob.h - a part of driver for BeBoB based devices
 *
 * Copyright (c) 2013-2014 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#ifndef SOUND_BEBOB_H_INCLUDED
#define SOUND_BEBOB_H_INCLUDED

#include <linux/compat.h>
#include <linux/device.h>
#include <linux/firewire.h>
#include <linux/firewire-constants.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/initval.h>

#include "../lib.h"
#include "../fcp.h"
#include "../packets-buffer.h"
#include "../iso-resources.h"
#include "../amdtp.h"
#include "../cmp.h"

/* basic register addresses on DM1000/DM1100/DM1500 */
#define BEBOB_ADDR_REG_INFO	0xffffc8020000
#define BEBOB_ADDR_REG_REQ	0xffffc8021000

struct snd_bebob;

#define SND_BEBOB_STRM_FMT_ENTRIES	7
struct snd_bebob_stream_formation {
	unsigned int pcm;
	unsigned int midi;
};
/* this is a lookup table for index of stream formations */
extern const unsigned int snd_bebob_rate_table[SND_BEBOB_STRM_FMT_ENTRIES];

struct snd_bebob {
	struct snd_card *card;
	struct fw_unit *unit;
	int card_index;

	struct mutex mutex;
	spinlock_t lock;

	unsigned int midi_input_ports;
	unsigned int midi_output_ports;

	struct amdtp_stream *master;
	struct amdtp_stream tx_stream;
	struct amdtp_stream rx_stream;
	struct cmp_connection out_conn;
	struct cmp_connection in_conn;
	atomic_t capture_substreams;
	atomic_t playback_substreams;

	struct snd_bebob_stream_formation
		tx_stream_formations[SND_BEBOB_STRM_FMT_ENTRIES];
	struct snd_bebob_stream_formation
		rx_stream_formations[SND_BEBOB_STRM_FMT_ENTRIES];

	int sync_input_plug;
};

static inline int
snd_bebob_read_block(struct fw_unit *unit, u64 addr, void *buf, int size)
{
	return snd_fw_transaction(unit, TCODE_READ_BLOCK_REQUEST,
				  BEBOB_ADDR_REG_INFO + addr,
				  buf, size, 0);
}

static inline int
snd_bebob_read_quad(struct fw_unit *unit, u64 addr, u32 *buf)
{
	return snd_fw_transaction(unit, TCODE_READ_QUADLET_REQUEST,
				  BEBOB_ADDR_REG_INFO + addr,
				  (void *)buf, sizeof(u32), 0);
}

/*
 * AVC command extensions, AV/C Unit and Subunit, Revision 17
 * (Nov 2003, BridgeCo)
 */
#define	AVC_BRIDGECO_ADDR_BYTES	6
enum avc_bridgeco_plug_dir {
	AVC_BRIDGECO_PLUG_DIR_IN	= 0x00,
	AVC_BRIDGECO_PLUG_DIR_OUT	= 0x01
};
enum avc_bridgeco_plug_mode {
	AVC_BRIDGECO_PLUG_MODE_UNIT		= 0x00,
	AVC_BRIDGECO_PLUG_MODE_SUBUNIT		= 0x01,
	AVC_BRIDGECO_PLUG_MODE_FUNCTION_BLOCK	= 0x02
};
enum avc_bridgeco_plug_unit {
	AVC_BRIDGECO_PLUG_UNIT_ISOC	= 0x00,
	AVC_BRIDGECO_PLUG_UNIT_EXT	= 0x01,
	AVC_BRIDGECO_PLUG_UNIT_ASYNC	= 0x02
};
enum avc_bridgeco_plug_type {
	AVC_BRIDGECO_PLUG_TYPE_ISOC	= 0x00,
	AVC_BRIDGECO_PLUG_TYPE_ASYNC	= 0x01,
	AVC_BRIDGECO_PLUG_TYPE_MIDI	= 0x02,
	AVC_BRIDGECO_PLUG_TYPE_SYNC	= 0x03,
	AVC_BRIDGECO_PLUG_TYPE_ANA	= 0x04,
	AVC_BRIDGECO_PLUG_TYPE_DIG	= 0x05
};
static inline void
avc_bridgeco_fill_unit_addr(u8 buf[AVC_BRIDGECO_ADDR_BYTES],
			    enum avc_bridgeco_plug_dir dir,
			    enum avc_bridgeco_plug_unit unit,
			    unsigned int pid)
{
	buf[0] = 0xff;	/* Unit */
	buf[1] = dir;
	buf[2] = AVC_BRIDGECO_PLUG_MODE_UNIT;
	buf[3] = unit;
	buf[4] = 0xff & pid;
	buf[5] = 0xff;	/* reserved */
}
static inline void
avc_bridgeco_fill_msu_addr(u8 buf[AVC_BRIDGECO_ADDR_BYTES],
			   enum avc_bridgeco_plug_dir dir,
			   unsigned int pid)
{
	buf[0] = 0x60;	/* Music subunit */
	buf[1] = dir;
	buf[2] = AVC_BRIDGECO_PLUG_MODE_SUBUNIT;
	buf[3] = 0xff & pid;
	buf[4] = 0xff;	/* reserved */
	buf[5] = 0xff;	/* reserved */
}
int avc_bridgeco_get_plug_ch_pos(struct fw_unit *unit,
				 u8 addr[AVC_BRIDGECO_ADDR_BYTES],
				 u8 *buf, unsigned int len);
int avc_bridgeco_get_plug_type(struct fw_unit *unit,
			       u8 addr[AVC_BRIDGECO_ADDR_BYTES],
			       enum avc_bridgeco_plug_type *type);
int avc_bridgeco_get_plug_section_type(struct fw_unit *unit,
				       u8 addr[AVC_BRIDGECO_ADDR_BYTES],
				       unsigned int id, u8 *type);
int avc_bridgeco_get_plug_input(struct fw_unit *unit,
				u8 addr[AVC_BRIDGECO_ADDR_BYTES],
				u8 input[7]);
int avc_bridgeco_get_plug_strm_fmt(struct fw_unit *unit,
				   u8 addr[AVC_BRIDGECO_ADDR_BYTES], u8 *buf,
				   unsigned int *len, unsigned int eid);

/* for AMDTP streaming */
int snd_bebob_stream_get_rate(struct snd_bebob *bebob, unsigned int *rate);
int snd_bebob_stream_set_rate(struct snd_bebob *bebob, unsigned int rate);
int snd_bebob_stream_check_internal_clock(struct snd_bebob *bebob,
					  bool *internal);
int snd_bebob_stream_discover(struct snd_bebob *bebob);
int snd_bebob_stream_map(struct snd_bebob *bebob,
			 struct amdtp_stream *stream);
int snd_bebob_stream_init_duplex(struct snd_bebob *bebob);
int snd_bebob_stream_start_duplex(struct snd_bebob *bebob, int rate);
void snd_bebob_stream_stop_duplex(struct snd_bebob *bebob);
void snd_bebob_stream_update_duplex(struct snd_bebob *bebob);
void snd_bebob_stream_destroy_duplex(struct snd_bebob *bebob);

#define SND_BEBOB_DEV_ENTRY(vendor, model) \
{ \
	.match_flags	= IEEE1394_MATCH_VENDOR_ID | \
			  IEEE1394_MATCH_MODEL_ID, \
	.vendor_id	= vendor, \
	.model_id	= model, \
}

#endif
