/*
 * bebob_terratek.c - a part of driver for BeBoB based devices
 *
 * Copyright (c) 2013 Takashi Sakamoto
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver; if not, see <http://www.gnu.org/licenses/>.
 */

#include "./bebob.h"

static char *phase88_rack_clock_labels[] = {
	"Internal", "Digital In", "Word Clock"
};
static int
phase88_rack_clock_get(struct snd_bebob *bebob, int *id)
{
	int enable_ext, enable_word, err;

	err = avc_audio_get_selector(bebob->unit, 0, 0, &enable_ext);
	if (err < 0)
		goto end;
	err = avc_audio_get_selector(bebob->unit, 0, 0, &enable_word);
	if (err < 0)
		goto end;

	*id = (enable_ext & 0x01) || ((enable_word & 0x01) << 1);
end:
	return err;
}
static int
phase88_rack_clock_set(struct snd_bebob *bebob, int id)
{
	int enable_ext, enable_word, err;

	enable_ext = id & 0x01;
	enable_word = (id >> 1) & 0x01;

	err = avc_audio_set_selector(bebob->unit, 0, 9, enable_ext);
	if (err < 0)
		goto end;
	err = avc_audio_set_selector(bebob->unit, 0, 8, enable_word);
	if (err < 0)
		goto end;
end:
	return err;
}

static int
phase88_rack_clock_synced(struct snd_bebob *bebob, bool *synced)
{
	int err;
	u8 *buf;

	buf = kmalloc(8, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0]  = 0x01;	/* STATUS */
	buf[1]  = 0xff;	/* UNIT */
	buf[2]  = 0x00;	/* Vendor Dependent */
	buf[3]  = 0x00; /* Company ID high */
	buf[4]  = 0x0a;	/* Company ID middle */
	buf[5]  = 0xac; /* Company ID low */
	buf[6]  = 0x21;	/* subfunction */
	buf[7]  = 0xff;	/* the state */

	err = fcp_avc_transaction(bebob->unit, buf, 8, buf, 8, 0);
	if (err < 0)
		goto end;
	/* IMPLEMENTED/STABLE is OK */
	if ((err < 6) || (buf[0] != 0x0c)) {
		dev_err(&bebob->unit->device,
			"fail to execute Terratek command: %02X\n",
			buf[0]);
		err = -EIO;
		goto end;
	}

	*synced = (buf[7] = 0x01);
end:
	kfree(buf);
	return err;
}

static char *phase24_series_clock_labels[] = {
	"Internal", "Digital In"
};
static int
phase24_series_clock_get(struct snd_bebob *bebob, int *id)
{
	return avc_audio_get_selector(bebob->unit, 0, 4, id);
}
static int
phase24_series_clock_set(struct snd_bebob *bebob, int id)
{
	return avc_audio_set_selector(bebob->unit, 0, 4, id);
}

struct snd_bebob_freq_spec freq_spec = {
	.get	= &snd_bebob_stream_get_rate,
	.set	= &snd_bebob_stream_set_rate
};

/* PHASE 88 Rack FW */
struct snd_bebob_clock_spec phase88_rack_clock = {
	.num	= ARRAY_SIZE(phase88_rack_clock_labels),
	.labels	= phase88_rack_clock_labels,
	.get	= &phase88_rack_clock_get,
	.set	= &phase88_rack_clock_set,
	.synced	= &phase88_rack_clock_synced
};
struct snd_bebob_spec phase88_rack_spec = {
	.load		= NULL,
	.discover	= &snd_bebob_stream_discover,
	.map		= &snd_bebob_stream_map,
	.freq		= &freq_spec,
	.clock		= &phase88_rack_clock,
	.dig_iface	= NULL,
	.meter		= NULL
};

/* 'PHASE 24 FW' and 'PHASE X24 FW' */
struct snd_bebob_clock_spec phase24_series_clock = {
	.num	= ARRAY_SIZE(phase24_series_clock_labels),
	.labels	= phase24_series_clock_labels,
	.get	= &phase24_series_clock_get,
	.set	= &phase24_series_clock_set,
	.synced	= NULL
};
struct snd_bebob_spec phase24_series_spec = {
	.load		= NULL,
	.discover	= &snd_bebob_stream_discover,
	.map		= &snd_bebob_stream_map,
	.freq		= &freq_spec,
	.clock		= &phase24_series_clock,
	.dig_iface	= NULL,
	.meter		= NULL
};
