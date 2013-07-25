/*
 * bebob_stream.c - driver for BeBoB based devices
 *
 * Copyright (c) 2013 Takashi Sakamoto
 *
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

int snd_bebob_strem_get_formation_index(int sampling_rate)
{
        int table[] = {
                [0] = 22050,
                [1] = 24000,
                [2] = 32000,
                [3] = 44100,
                [4] = 48000,
                [5] = 88200,
                [6] = 96000,
                [7] = 176400,
                [8] = 192000,
        };
        int i;

        for (i = 0; i < sizeof(table); i += 1) {
                if (table[i] == sampling_rate)
                        return i;
	}
	return -1;
}


int snd_bebob_stream_init(struct snd_bebob *bebob, struct amdtp_stream *stream)
{
	struct cmp_connection *connection;
	enum cmp_direction c_dir;
	enum amdtp_stream_direction s_dir;
	int err;

	if (stream == &bebob->tx_stream) {
		connection = &bebob->output_connection;
		c_dir = CMP_OUTPUT;
		s_dir = AMDTP_IN_STREAM;
	} else {
		connection = &bebob->input_connection;
		c_dir = CMP_INPUT;
		s_dir = AMDTP_OUT_STREAM;
	}

	err = cmp_connection_init(connection, bebob->unit, c_dir, 0);
	if (err < 0)
		goto end;

	err = amdtp_stream_init(stream, bebob->unit, s_dir, CIP_NONBLOCKING);
	if (err < 0) {
		cmp_connection_destroy(connection);
		goto end;

	}

end:
	return err;
}

int snd_bebob_stream_start(struct snd_bebob *bebob, struct amdtp_stream *stream,
			   unsigned int sampling_rate)
{
	struct snd_bebob_stream_formation *formations;
	struct cmp_connection *connection;
	unsigned int i, pcm_channels, midi_channels;
	int err = 0;

	/* already running */
	if (!IS_ERR(stream->context))
		goto end;

	if (stream == &bebob->tx_stream) {
		formations = bebob->tx_stream_formations;
		connection = &bebob->output_connection;
	} else {
		formations = bebob->rx_stream_formations;
		connection = &bebob->input_connection;
	}

	pcm_channels = 0;
	midi_channels = 0;
	for (i = 0; i < SND_BEBOB_STREAM_FORMATION_ENTRIES; i++) {
		if (formations[i].sampling_rate != sampling_rate)
			continue;
		pcm_channels = formations[i].pcm;
		midi_channels = formations[i].midi;
		break;
	}

	amdtp_stream_set_params(stream, sampling_rate,
				pcm_channels, midi_channels);

	/*  establish connection via CMP */
	err = cmp_connection_establish(connection,
				amdtp_stream_get_max_payload(stream));
	if (err < 0)
		goto end;

	/* start amdtp stream */
	err = amdtp_stream_start(stream,
				 connection->resources.channel,
				 connection->speed);
	if (err < 0)
		cmp_connection_break(connection);

end:
	return err;
}

void snd_bebob_stream_stop(struct snd_bebob *bebob, struct amdtp_stream *stream)
{
	if (!!IS_ERR(stream->context))
		goto end;

	amdtp_stream_stop(stream);

	if (stream == &bebob->tx_stream)
		cmp_connection_break(&bebob->output_connection);
	else
		cmp_connection_break(&bebob->input_connection);
end:
	return;
}

void snd_bebob_stream_destroy(struct snd_bebob *bebob, struct amdtp_stream *stream)
{
	snd_bebob_stream_stop(bebob, stream);

	if (stream == &bebob->tx_stream)
		cmp_connection_destroy(&bebob->output_connection);
	else
		cmp_connection_destroy(&bebob->input_connection);

	return;
}
