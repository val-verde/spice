/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <common/generated_server_marshallers.h>
#include <common/snd_codec.h>

#include "spice.h"
#include "red-common.h"
#include "main-channel.h"
#include "reds.h"
#include "red-qxl.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "sound.h"
#include "main-channel-client.h"

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#define SND_RECEIVE_BUF_SIZE     (16 * 1024 * 2)
#define RECORD_SAMPLES_SIZE (SND_RECEIVE_BUF_SIZE >> 2)

enum SndCommand {
    SND_MIGRATE,
    SND_CTRL,
    SND_VOLUME,
    SND_MUTE,
    SND_END_COMMAND,
};

enum PlaybackCommand {
    SND_PLAYBACK_MODE = SND_END_COMMAND,
    SND_PLAYBACK_PCM,
    SND_PLAYBACK_LATENCY,
};

#define SND_MIGRATE_MASK (1 << SND_MIGRATE)
#define SND_CTRL_MASK (1 << SND_CTRL)
#define SND_VOLUME_MASK (1 << SND_VOLUME)
#define SND_MUTE_MASK (1 << SND_MUTE)
#define SND_VOLUME_MUTE_MASK (SND_VOLUME_MASK|SND_MUTE_MASK)

#define SND_PLAYBACK_MODE_MASK (1 << SND_PLAYBACK_MODE)
#define SND_PLAYBACK_PCM_MASK (1 << SND_PLAYBACK_PCM)
#define SND_PLAYBACK_LATENCY_MASK ( 1 << SND_PLAYBACK_LATENCY)

typedef struct SndChannelClient SndChannelClient;
typedef struct SndChannel SndChannel;
typedef struct PlaybackChannelClient PlaybackChannelClient;
typedef struct RecordChannelClient RecordChannelClient;
typedef struct AudioFrame AudioFrame;
typedef struct AudioFrameContainer AudioFrameContainer;
typedef struct SpicePlaybackState PlaybackChannel;
typedef struct SpiceRecordState RecordChannel;

typedef void (*snd_channel_on_message_done_proc)(SndChannelClient *client);


#define TYPE_SND_CHANNEL_CLIENT snd_channel_client_get_type()
#define SND_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_SND_CHANNEL_CLIENT, SndChannelClient))
GType snd_channel_client_get_type(void) G_GNUC_CONST;

/* Connects an audio client to a Spice client */
struct SndChannelClient {
    RedChannelClient parent;

    int active;
    int client_active;

    uint32_t command;

    /* we don't expect very big messages so don't allocate too much
     * bytes, data will be cached in RecordChannelClient::samples */
    uint8_t receive_buf[SND_CODEC_MAX_FRAME_BYTES + 64];
    RedPipeItem persistent_pipe_item;

    snd_channel_on_message_done_proc on_message_done;
};

typedef struct SndChannelClientClass {
    RedChannelClientClass parent_class;
} SndChannelClientClass;

G_DEFINE_TYPE(SndChannelClient, snd_channel_client, RED_TYPE_CHANNEL_CLIENT)


enum {
    RED_PIPE_ITEM_PERSISTENT = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
};


struct AudioFrame {
    uint32_t time;
    uint32_t samples[SND_CODEC_MAX_FRAME_SIZE];
    PlaybackChannelClient *client;
    AudioFrame *next;
    AudioFrameContainer *container;
    gboolean allocated;
};

#define NUM_AUDIO_FRAMES 3
struct AudioFrameContainer
{
    int refs;
    AudioFrame items[NUM_AUDIO_FRAMES];
};

#define TYPE_PLAYBACK_CHANNEL_CLIENT playback_channel_client_get_type()
#define PLAYBACK_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_PLAYBACK_CHANNEL_CLIENT, PlaybackChannelClient))
GType playback_channel_client_get_type(void) G_GNUC_CONST;

struct PlaybackChannelClient {
    SndChannelClient parent;

    AudioFrameContainer *frames;
    AudioFrame *free_frames;
    AudioFrame *in_progress;   /* Frame being sent to the client */
    AudioFrame *pending_frame; /* Next frame to send to the client */
    uint32_t mode;
    uint32_t latency;
    SndCodec codec;
    uint8_t  encode_buf[SND_CODEC_MAX_COMPRESSED_BYTES];
};

typedef struct PlaybackChannelClientClass {
    SndChannelClientClass parent_class;
} PlaybackChannelClientClass;

G_DEFINE_TYPE(PlaybackChannelClient, playback_channel_client, TYPE_SND_CHANNEL_CLIENT)


typedef struct SpiceVolumeState {
    uint16_t *volume;
    uint8_t volume_nchannels;
    int mute;
} SpiceVolumeState;

#define TYPE_SND_CHANNEL snd_channel_get_type()
#define SND_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_SND_CHANNEL, SndChannel))
GType snd_channel_get_type(void) G_GNUC_CONST;

/* Base class for SpicePlaybackState and SpiceRecordState */
struct SndChannel {
    RedChannel parent;

    SndChannelClient *connection; /* Only one client is supported */
    SndChannel *next; /* For the global SndChannel list */

    int active;
    SpiceVolumeState volume;
    uint32_t frequency;
};

typedef struct SndChannelClass {
    RedChannelClass parent_class;
} SndChannelClass;

G_DEFINE_TYPE(SndChannel, snd_channel, RED_TYPE_CHANNEL)


#define TYPE_PLAYBACK_CHANNEL playback_channel_get_type()
#define PLAYBACK_CHANNEL(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_PLAYBACK_CHANNEL, PlaybackChannel))
GType playback_channel_get_type(void) G_GNUC_CONST;

struct SpicePlaybackState {
    SndChannel channel;
};

typedef struct PlaybackChannelClass {
    SndChannelClass parent_class;
} PlaybackChannelClass;

G_DEFINE_TYPE(PlaybackChannel, playback_channel, TYPE_SND_CHANNEL)


#define TYPE_RECORD_CHANNEL record_channel_get_type()
#define RECORD_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_RECORD_CHANNEL, RecordChannel))
GType record_channel_get_type(void) G_GNUC_CONST;

struct SpiceRecordState {
    SndChannel channel;
};

typedef struct RecordChannelClass {
    SndChannelClass parent_class;
} RecordChannelClass;

G_DEFINE_TYPE(RecordChannel, record_channel, TYPE_SND_CHANNEL)


#define TYPE_RECORD_CHANNEL_CLIENT record_channel_client_get_type()
#define RECORD_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_RECORD_CHANNEL_CLIENT, RecordChannelClient))
GType record_channel_client_get_type(void) G_GNUC_CONST;

struct RecordChannelClient {
    SndChannelClient parent;
    uint32_t samples[RECORD_SAMPLES_SIZE];
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t mode;
    uint32_t mode_time;
    uint32_t start_time;
    SndCodec codec;
    uint8_t  decode_buf[SND_CODEC_MAX_FRAME_BYTES];
};

typedef struct RecordChannelClientClass {
    SndChannelClientClass parent_class;
} RecordChannelClientClass;

G_DEFINE_TYPE(RecordChannelClient, record_channel_client, TYPE_SND_CHANNEL_CLIENT)


/* A list of all Spice{Playback,Record}State objects */
static SndChannel *snd_channels;

static void snd_playback_start(SndChannel *channel);
static void snd_record_start(SndChannel *channel);
static void snd_send(SndChannelClient * client);

static RedsState* snd_channel_get_server(SndChannelClient *client)
{
    g_return_val_if_fail(client != NULL, NULL);
    return red_channel_get_server(red_channel_client_get_channel(RED_CHANNEL_CLIENT(client)));
}

static void snd_playback_free_frame(PlaybackChannelClient *playback_client, AudioFrame *frame)
{
    frame->client = playback_client;
    frame->next = playback_client->free_frames;
    playback_client->free_frames = frame;
}

static void snd_playback_on_message_done(SndChannelClient *client)
{
    PlaybackChannelClient *playback_client = (PlaybackChannelClient *)client;
    if (playback_client->in_progress) {
        snd_playback_free_frame(playback_client, playback_client->in_progress);
        playback_client->in_progress = NULL;
        if (playback_client->pending_frame) {
            client->command |= SND_PLAYBACK_PCM_MASK;
            snd_send(client);
        }
    }
}

static int snd_record_handle_write(RecordChannelClient *record_client, size_t size, void *message)
{
    SpiceMsgcRecordPacket *packet;
    uint32_t write_pos;
    uint32_t* data;
    uint32_t len;
    uint32_t now;

    if (!record_client) {
        return FALSE;
    }

    packet = (SpiceMsgcRecordPacket *)message;

    if (record_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        data = (uint32_t *)packet->data;
        size = packet->data_size >> 2;
        size = MIN(size, RECORD_SAMPLES_SIZE);
     } else {
        int decode_size;
        decode_size = sizeof(record_client->decode_buf);
        if (snd_codec_decode(record_client->codec, packet->data, packet->data_size,
                    record_client->decode_buf, &decode_size) != SND_CODEC_OK)
            return FALSE;
        data = (uint32_t *) record_client->decode_buf;
        size = decode_size >> 2;
    }

    write_pos = record_client->write_pos % RECORD_SAMPLES_SIZE;
    record_client->write_pos += size;
    len = RECORD_SAMPLES_SIZE - write_pos;
    now = MIN(len, size);
    size -= now;
    memcpy(record_client->samples + write_pos, data, now << 2);

    if (size) {
        memcpy(record_client->samples, data + now, size << 2);
    }

    if (record_client->write_pos - record_client->read_pos > RECORD_SAMPLES_SIZE) {
        record_client->read_pos = record_client->write_pos - RECORD_SAMPLES_SIZE;
    }
    return TRUE;
}

static int
playback_channel_handle_parsed(RedChannelClient *rcc, uint32_t size, uint16_t type, void *message)
{
    switch (type) {
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
    return TRUE;
}

static int
record_channel_handle_parsed(RedChannelClient *rcc, uint32_t size, uint16_t type, void *message)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(rcc);

    switch (type) {
    case SPICE_MSGC_RECORD_DATA:
        return snd_record_handle_write(record_client, size, message);
    case SPICE_MSGC_RECORD_MODE: {
        SpiceMsgcRecordMode *mode = (SpiceMsgcRecordMode *)message;
        SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
        record_client->mode_time = mode->time;
        if (mode->mode != SPICE_AUDIO_DATA_MODE_RAW) {
            if (snd_codec_is_capable(mode->mode, channel->frequency)) {
                if (snd_codec_create(&record_client->codec, mode->mode, channel->frequency,
                                     SND_CODEC_DECODE) == SND_CODEC_OK) {
                    record_client->mode = mode->mode;
                } else {
                    spice_printerr("create decoder failed");
                    return FALSE;
                }
            }
            else {
                spice_printerr("unsupported mode %d", record_client->mode);
                return FALSE;
            }
        }
        else
            record_client->mode = mode->mode;
        break;
    }

    case SPICE_MSGC_RECORD_START_MARK: {
        SpiceMsgcRecordStartMark *mark = (SpiceMsgcRecordStartMark *)message;
        record_client->start_time = mark->time;
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
    return TRUE;
}

static int snd_channel_send_migrate(SndChannelClient *client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgMigrate migrate;

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE);
    migrate.flags = 0;
    spice_marshall_msg_migrate(m, &migrate);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_migrate(PlaybackChannelClient *client)
{
    return snd_channel_send_migrate(SND_CHANNEL_CLIENT(client));
}

static int snd_send_volume(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioVolume *vol;
    uint8_t c;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceVolumeState *st = &channel->volume;

    if (!red_channel_client_test_remote_cap(rcc, cap)) {
        return FALSE;
    }

    vol = alloca(sizeof (SpiceMsgAudioVolume) +
                 st->volume_nchannels * sizeof (uint16_t));
    red_channel_client_init_send_data(rcc, msg);
    vol->nchannels = st->volume_nchannels;
    for (c = 0; c < st->volume_nchannels; ++c) {
        vol->volume[c] = st->volume[c];
    }
    spice_marshall_SpiceMsgAudioVolume(m, vol);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_volume(PlaybackChannelClient *playback_client)
{
    return snd_send_volume(SND_CHANNEL_CLIENT(playback_client), SPICE_PLAYBACK_CAP_VOLUME,
                           SPICE_MSG_PLAYBACK_VOLUME);
}

static int snd_send_mute(SndChannelClient *client, uint32_t cap, int msg)
{
    SpiceMsgAudioMute mute;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceVolumeState *st = &channel->volume;

    if (!red_channel_client_test_remote_cap(rcc, cap)) {
        return FALSE;
    }

    red_channel_client_init_send_data(rcc, msg);
    mute.mute = st->mute;
    spice_marshall_SpiceMsgAudioMute(m, &mute);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_mute(PlaybackChannelClient *playback_client)
{
    return snd_send_mute(SND_CHANNEL_CLIENT(playback_client), SPICE_PLAYBACK_CAP_VOLUME,
                         SPICE_MSG_PLAYBACK_MUTE);
}

static int snd_playback_send_latency(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackLatency latency_msg;

    spice_debug("latency %u", playback_client->latency);
    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_LATENCY);
    latency_msg.latency_ms = playback_client->latency;
    spice_marshall_msg_playback_latency(m, &latency_msg);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_start(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackStart start;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_START);
    start.channels = SPICE_INTERFACE_PLAYBACK_CHAN;
    start.frequency = SND_CHANNEL(red_channel_client_get_channel(rcc))->frequency;
    spice_assert(SPICE_INTERFACE_PLAYBACK_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    start.time = reds_get_mm_time();
    spice_marshall_msg_playback_start(m, &start);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_stop(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_STOP);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_playback_send_ctl(PlaybackChannelClient *playback_client)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(playback_client);

    if ((client->client_active = client->active)) {
        return snd_playback_send_start(playback_client);
    } else {
        return snd_playback_send_stop(playback_client);
    }
}

static int snd_record_send_start(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(record_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgRecordStart start;

    red_channel_client_init_send_data(rcc, SPICE_MSG_RECORD_START);

    start.channels = SPICE_INTERFACE_RECORD_CHAN;
    start.frequency = SND_CHANNEL(red_channel_client_get_channel(rcc))->frequency;
    spice_assert(SPICE_INTERFACE_RECORD_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    spice_marshall_msg_record_start(m, &start);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_record_send_stop(RecordChannelClient *record_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(record_client);

    red_channel_client_init_send_data(rcc, SPICE_MSG_RECORD_STOP);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int snd_record_send_ctl(RecordChannelClient *record_client)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(record_client);

    if ((client->client_active = client->active)) {
        return snd_record_send_start(record_client);
    } else {
        return snd_record_send_stop(record_client);
    }
}

static int snd_record_send_volume(RecordChannelClient *record_client)
{
    return snd_send_volume(SND_CHANNEL_CLIENT(record_client), SPICE_RECORD_CAP_VOLUME,
                           SPICE_MSG_RECORD_VOLUME);
}

static int snd_record_send_mute(RecordChannelClient *record_client)
{
    return snd_send_mute(SND_CHANNEL_CLIENT(record_client), SPICE_RECORD_CAP_VOLUME,
                         SPICE_MSG_RECORD_MUTE);
}

static int snd_record_send_migrate(RecordChannelClient *record_client)
{
    /* No need for migration data: if recording has started before migration,
     * the client receives RECORD_STOP from the src before the migration completion
     * notification (when the vm is stopped).
     * Afterwards, when the vm starts on the dest, the client receives RECORD_START. */
    return snd_channel_send_migrate(SND_CHANNEL_CLIENT(record_client));
}

static int snd_playback_send_write(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    AudioFrame *frame;
    SpiceMsgPlaybackPacket msg;
    RedPipeItem *pipe_item = &SND_CHANNEL_CLIENT(playback_client)->persistent_pipe_item;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_DATA);

    frame = playback_client->in_progress;
    msg.time = frame->time;

    spice_marshall_msg_playback_data(m, &msg);

    if (playback_client->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        spice_marshaller_add_by_ref_full(m, (uint8_t *)frame->samples,
                                         snd_codec_frame_size(playback_client->codec) *
                                         sizeof(frame->samples[0]),
                                         marshaller_unref_pipe_item, pipe_item);
    }
    else {
        int n = sizeof(playback_client->encode_buf);
        if (snd_codec_encode(playback_client->codec, (uint8_t *) frame->samples,
                                    snd_codec_frame_size(playback_client->codec) * sizeof(frame->samples[0]),
                                    playback_client->encode_buf, &n) != SND_CODEC_OK) {
            spice_printerr("encode failed");
            red_channel_client_disconnect(rcc);
            return FALSE;
        }
        spice_marshaller_add_by_ref_full(m, playback_client->encode_buf, n,
                                         marshaller_unref_pipe_item, pipe_item);
    }

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

static int playback_send_mode(PlaybackChannelClient *playback_client)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    SpiceMsgPlaybackMode mode;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PLAYBACK_MODE);
    mode.time = reds_get_mm_time();
    mode.mode = playback_client->mode;
    spice_marshall_msg_playback_mode(m, &mode);

    red_channel_client_begin_send_message(rcc);
    return TRUE;
}

/* This function is called when the "persistent" item is removed from the
 * queue. Note that there is not free call as the item is allocated into
 * SndChannelClient.
 * This is used to have a simple item in RedChannelClient queue but to send
 * multiple messages in a row if possible.
 * During realtime sound transmission you usually don't want to queue too
 * much data or having retransmission preferring instead loosing some
 * samples.
 */
static void snd_persistent_pipe_item_free(struct RedPipeItem *item)
{
    SndChannelClient *client = SPICE_CONTAINEROF(item, SndChannelClient, persistent_pipe_item);

    red_pipe_item_init_full(item, RED_PIPE_ITEM_PERSISTENT,
                            snd_persistent_pipe_item_free);

    if (client->on_message_done) {
        client->on_message_done(client);
    }
}

static void snd_send(SndChannelClient * client)
{
    RedChannelClient *rcc;

    g_return_if_fail(RED_IS_CHANNEL_CLIENT(client));

    rcc = RED_CHANNEL_CLIENT(client);
    if (!red_channel_client_pipe_is_empty(rcc) || !client->command) {
        return;
    }
    // just append a dummy item and push!
    red_pipe_item_init_full(&client->persistent_pipe_item, RED_PIPE_ITEM_PERSISTENT,
                            snd_persistent_pipe_item_free);
    red_channel_client_pipe_add_push(rcc, &client->persistent_pipe_item);
}

static void playback_channel_send_item(RedChannelClient *rcc, G_GNUC_UNUSED RedPipeItem *item)
{
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(rcc);
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);

    client->command &= SND_PLAYBACK_MODE_MASK|SND_PLAYBACK_PCM_MASK|
                       SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|
                       SND_MIGRATE_MASK|SND_PLAYBACK_LATENCY_MASK;
    while (client->command) {
        if (client->command & SND_PLAYBACK_MODE_MASK) {
            client->command &= ~SND_PLAYBACK_MODE_MASK;
            if (playback_send_mode(playback_client)) {
                break;
            }
        }
        if (client->command & SND_PLAYBACK_PCM_MASK) {
            spice_assert(!playback_client->in_progress && playback_client->pending_frame);
            playback_client->in_progress = playback_client->pending_frame;
            playback_client->pending_frame = NULL;
            client->command &= ~SND_PLAYBACK_PCM_MASK;
            if (snd_playback_send_write(playback_client)) {
                break;
            }
            spice_printerr("snd_send_playback_write failed");
        }
        if (client->command & SND_CTRL_MASK) {
            client->command &= ~SND_CTRL_MASK;
            if (snd_playback_send_ctl(playback_client)) {
                break;
            }
        }
        if (client->command & SND_VOLUME_MASK) {
            client->command &= ~SND_VOLUME_MASK;
            if (snd_playback_send_volume(playback_client)) {
                break;
            }
        }
        if (client->command & SND_MUTE_MASK) {
            client->command &= ~SND_MUTE_MASK;
            if (snd_playback_send_mute(playback_client)) {
                break;
            }
        }
        if (client->command & SND_MIGRATE_MASK) {
            client->command &= ~SND_MIGRATE_MASK;
            if (snd_playback_send_migrate(playback_client)) {
                break;
            }
        }
        if (client->command & SND_PLAYBACK_LATENCY_MASK) {
            client->command &= ~SND_PLAYBACK_LATENCY_MASK;
            if (snd_playback_send_latency(playback_client)) {
                break;
            }
        }
    }
    snd_send(client);
}

static void record_channel_send_item(RedChannelClient *rcc, G_GNUC_UNUSED RedPipeItem *item)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(rcc);
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);

    client->command &= SND_CTRL_MASK|SND_VOLUME_MUTE_MASK|SND_MIGRATE_MASK;
    while (client->command) {
        if (client->command & SND_CTRL_MASK) {
            client->command &= ~SND_CTRL_MASK;
            if (snd_record_send_ctl(record_client)) {
                break;
            }
        }
        if (client->command & SND_VOLUME_MASK) {
            client->command &= ~SND_VOLUME_MASK;
            if (snd_record_send_volume(record_client)) {
                break;
            }
        }
        if (client->command & SND_MUTE_MASK) {
            client->command &= ~SND_MUTE_MASK;
            if (snd_record_send_mute(record_client)) {
                break;
            }
        }
        if (client->command & SND_MIGRATE_MASK) {
            client->command &= ~SND_MIGRATE_MASK;
            if (snd_record_send_migrate(record_client)) {
                break;
            }
        }
    }
    snd_send(client);
}

static int snd_channel_config_socket(RedChannelClient *rcc)
{
    int delay_val;
    int flags;
#ifdef SO_PRIORITY
    int priority;
#endif
    int tos;
    RedsStream *stream = red_channel_client_get_stream(rcc);
    RedClient *red_client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(red_client);

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        spice_printerr("accept failed, %s", strerror(errno));
        return FALSE;
    }

#ifdef SO_PRIORITY
    priority = 6;
    if (setsockopt(stream->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }
#endif

    tos = IPTOS_LOWDELAY;
    if (setsockopt(stream->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }

    delay_val = main_channel_client_is_low_bandwidth(mcc) ? 0 : 1;
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        spice_printerr("accept failed, %s", strerror(errno));
        return FALSE;
    }

    return TRUE;
}

static void snd_channel_on_disconnect(RedChannelClient *rcc)
{
    SndChannel *channel = SND_CHANNEL(red_channel_client_get_channel(rcc));
    if (channel->connection && rcc == RED_CHANNEL_CLIENT(channel->connection)) {
        channel->connection = NULL;
    }
}

static uint8_t*
snd_channel_client_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);
    // If message is too big allocate one, this should never happen
    if (size > sizeof(client->receive_buf)) {
        return spice_malloc(size);
    }
    return client->receive_buf;
}

static void
snd_channel_client_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t *msg)
{
    SndChannelClient *client = SND_CHANNEL_CLIENT(rcc);
    if (msg != client->receive_buf) {
        free(msg);
    }
}

static void snd_disconnect_channel_client(RedChannelClient *rcc)
{
    SndChannel *channel;
    RedChannel *red_channel = red_channel_client_get_channel(rcc);
    uint32_t type;

    channel = SND_CHANNEL(red_channel);
    spice_assert(channel);
    g_object_get(red_channel, "channel-type", &type, NULL);

    spice_debug("channel-type=%d", type);
    if (channel->connection) {
        spice_assert(RED_CHANNEL_CLIENT(channel->connection) == rcc);
        red_channel_client_disconnect(rcc);
    }
}

static void snd_set_command(SndChannelClient *client, uint32_t command)
{
    if (!client) {
        return;
    }
    client->command |= command;
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_volume(SpicePlaybackInstance *sin,
                                                  uint8_t nchannels,
                                                  uint16_t *volume)
{
    SpiceVolumeState *st = &sin->st->channel.volume;
    SndChannelClient *client = sin->st->channel.connection;

    st->volume_nchannels = nchannels;
    free(st->volume);
    st->volume = spice_memdup(volume, sizeof(uint16_t) * nchannels);

    if (!client || nchannels == 0)
        return;

    snd_set_command(client, SND_VOLUME_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_mute(SpicePlaybackInstance *sin, uint8_t mute)
{
    SpiceVolumeState *st = &sin->st->channel.volume;
    SndChannelClient *client = sin->st->channel.connection;

    st->mute = mute;

    if (!client)
        return;

    snd_set_command(client, SND_MUTE_MASK);
    snd_send(client);
}

static void snd_playback_start(SndChannel *channel)
{
    SndChannelClient *client = channel->connection;

    channel->active = 1;
    if (!client)
        return;
    spice_assert(!client->active);
    reds_disable_mm_time(snd_channel_get_server(client));
    client->active = TRUE;
    if (!client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_start(SpicePlaybackInstance *sin)
{
    return snd_playback_start(&sin->st->channel);
}

SPICE_GNUC_VISIBLE void spice_server_playback_stop(SpicePlaybackInstance *sin)
{
    SndChannelClient *client = sin->st->channel.connection;

    sin->st->channel.active = 0;
    if (!client)
        return;
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    spice_assert(client->active);
    reds_enable_mm_time(snd_channel_get_server(client));
    client->active = FALSE;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
        client->command &= ~SND_PLAYBACK_PCM_MASK;

        if (playback_client->pending_frame) {
            spice_assert(!playback_client->in_progress);
            snd_playback_free_frame(playback_client,
                                    playback_client->pending_frame);
            playback_client->pending_frame = NULL;
        }
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_get_buffer(SpicePlaybackInstance *sin,
                                                         uint32_t **frame, uint32_t *num_samples)
{
    SndChannelClient *client = sin->st->channel.connection;

    *frame = NULL;
    *num_samples = 0;
    if (!client) {
        return;
    }
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(client);
    if (!playback_client->free_frames) {
        return;
    }
    spice_assert(client->active);
    if (!playback_client->free_frames->allocated) {
        playback_client->free_frames->allocated = TRUE;
        ++playback_client->frames->refs;
    }

    *frame = playback_client->free_frames->samples;
    playback_client->free_frames = playback_client->free_frames->next;
    *num_samples = snd_codec_frame_size(playback_client->codec);
}

SPICE_GNUC_VISIBLE void spice_server_playback_put_samples(SpicePlaybackInstance *sin, uint32_t *samples)
{
    PlaybackChannelClient *playback_client;
    AudioFrame *frame;

    frame = SPICE_CONTAINEROF(samples, AudioFrame, samples[0]);
    if (frame->allocated) {
        frame->allocated = FALSE;
        if (--frame->container->refs == 0) {
            free(frame->container);
            return;
        }
    }
    playback_client = frame->client;
    if (!playback_client || sin->st->channel.connection != SND_CHANNEL_CLIENT(playback_client)) {
        /* lost last reference, client has been destroyed previously */
        spice_info("audio samples belong to a disconnected client");
        return;
    }
    spice_assert(SND_CHANNEL_CLIENT(playback_client)->active);

    if (playback_client->pending_frame) {
        snd_playback_free_frame(playback_client, playback_client->pending_frame);
    }
    frame->time = reds_get_mm_time();
    playback_client->pending_frame = frame;
    snd_set_command(SND_CHANNEL_CLIENT(playback_client), SND_PLAYBACK_PCM_MASK);
    snd_send(SND_CHANNEL_CLIENT(playback_client));
}

void snd_set_playback_latency(RedClient *client, uint32_t latency)
{
    SndChannel *now = snd_channels;

    for (; now; now = now->next) {
        uint32_t type;
        g_object_get(RED_CHANNEL(now), "channel-type", &type, NULL);
        if (type == SPICE_CHANNEL_PLAYBACK && now->connection &&
            red_channel_client_get_client(RED_CHANNEL_CLIENT(now->connection)) == client) {

            if (red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(now->connection),
                SPICE_PLAYBACK_CAP_LATENCY)) {
                PlaybackChannelClient* playback = (PlaybackChannelClient*)now->connection;

                playback->latency = latency;
                snd_set_command(now->connection, SND_PLAYBACK_LATENCY_MASK);
                snd_send(now->connection);
            } else {
                spice_debug("client doesn't not support SPICE_PLAYBACK_CAP_LATENCY");
            }
        }
    }
}

static int snd_desired_audio_mode(int playback_compression, int frequency,
                                  int client_can_celt, int client_can_opus)
{
    if (! playback_compression)
        return SPICE_AUDIO_DATA_MODE_RAW;

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        return SPICE_AUDIO_DATA_MODE_OPUS;

    if (client_can_celt && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, frequency))
        return SPICE_AUDIO_DATA_MODE_CELT_0_5_1;

    return SPICE_AUDIO_DATA_MODE_RAW;
}

static void on_new_playback_channel_client(SndChannel *channel, SndChannelClient *client)
{
    RedsState *reds = red_channel_get_server(RED_CHANNEL(channel));

    spice_assert(client);

    channel->connection = client;
    snd_set_command(client, SND_PLAYBACK_MODE_MASK);
    if (client->active) {
        snd_set_command(client, SND_CTRL_MASK);
    }
    if (channel->volume.volume_nchannels) {
        snd_set_command(client, SND_VOLUME_MUTE_MASK);
    }
    if (client->active) {
        reds_disable_mm_time(reds);
    }
}

static void
playback_channel_client_finalize(GObject *object)
{
    int i;
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(object);
    SndChannelClient *client = SND_CHANNEL_CLIENT(playback_client);

    // free frames, unref them
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        playback_client->frames->items[i].client = NULL;
    }
    if (--playback_client->frames->refs == 0) {
        free(playback_client->frames);
    }

    if (client->active) {
        reds_enable_mm_time(snd_channel_get_server(client));
    }

    snd_codec_destroy(&playback_client->codec);

    G_OBJECT_CLASS(playback_channel_client_parent_class)->finalize(object);
}

static void
playback_channel_client_constructed(GObject *object)
{
    PlaybackChannelClient *playback_client = PLAYBACK_CHANNEL_CLIENT(object);
    RedChannel *red_channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(playback_client));
    RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(playback_client));
    SndChannel *channel = SND_CHANNEL(red_channel);

    G_OBJECT_CLASS(playback_channel_client_parent_class)->constructed(object);

    SND_CHANNEL_CLIENT(playback_client)->on_message_done = snd_playback_on_message_done;

    RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback_client);
    int client_can_celt = red_channel_client_test_remote_cap(rcc,
                                          SPICE_PLAYBACK_CAP_CELT_0_5_1);
    int client_can_opus = red_channel_client_test_remote_cap(rcc,
                                          SPICE_PLAYBACK_CAP_OPUS);
    int playback_compression =
        reds_config_get_playback_compression(red_channel_get_server(red_channel));
    int desired_mode = snd_desired_audio_mode(playback_compression, channel->frequency,
                                              client_can_celt, client_can_opus);
    if (desired_mode != SPICE_AUDIO_DATA_MODE_RAW) {
        if (snd_codec_create(&playback_client->codec, desired_mode, channel->frequency,
                             SND_CODEC_ENCODE) == SND_CODEC_OK) {
            playback_client->mode = desired_mode;
        } else {
            spice_printerr("create encoder failed");
        }
    }

    if (!red_client_during_migrate_at_target(client)) {
        on_new_playback_channel_client(channel, SND_CHANNEL_CLIENT(playback_client));
    }

    if (channel->active) {
        snd_playback_start(channel);
    }
    snd_send(SND_CHANNEL_CLIENT(playback_client));
}

static void snd_set_playback_peer(RedChannel *red_channel, RedClient *client, RedsStream *stream,
                                  G_GNUC_UNUSED int migration,
                                  int num_common_caps, uint32_t *common_caps,
                                  int num_caps, uint32_t *caps)
{
    SndChannel *channel = SND_CHANNEL(red_channel);
    GArray *common_caps_array = NULL, *caps_array = NULL;
    PlaybackChannelClient *playback_client;

    if (channel->connection) {
        red_channel_client_disconnect(RED_CHANNEL_CLIENT(channel->connection));
        channel->connection = NULL;
    }

    if (common_caps) {
        common_caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*common_caps),
                                              num_common_caps);
        g_array_append_vals(common_caps_array, common_caps, num_common_caps);
    }
    if (caps) {
        caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*caps), num_caps);
        g_array_append_vals(caps_array, caps, num_caps);
    }

    playback_client = g_initable_new(TYPE_PLAYBACK_CHANNEL_CLIENT,
                                     NULL, NULL,
                                     "channel", channel,
                                     "client", client,
                                     "stream", stream,
                                     "caps", caps_array,
                                     "common-caps", common_caps_array,
                                     NULL);
    g_warn_if_fail(playback_client != NULL);

    if (caps_array) {
        g_array_unref(caps_array);
    }
    if (common_caps_array) {
        g_array_unref(common_caps_array);
    }
}

static void snd_record_migrate_channel_client(RedChannelClient *rcc)
{
    SndChannel *channel;
    RedChannel *red_channel = red_channel_client_get_channel(rcc);

    channel = SND_CHANNEL(red_channel);
    spice_assert(channel);

    if (channel->connection) {
        spice_assert(RED_CHANNEL_CLIENT(channel->connection) == rcc);
        snd_set_command(channel->connection, SND_MIGRATE_MASK);
        snd_send(channel->connection);
    }
}

SPICE_GNUC_VISIBLE void spice_server_record_set_volume(SpiceRecordInstance *sin,
                                                uint8_t nchannels,
                                                uint16_t *volume)
{
    SpiceVolumeState *st = &sin->st->channel.volume;
    SndChannelClient *client = sin->st->channel.connection;

    st->volume_nchannels = nchannels;
    free(st->volume);
    st->volume = spice_memdup(volume, sizeof(uint16_t) * nchannels);

    if (!client || nchannels == 0)
        return;

    snd_set_command(client, SND_VOLUME_MASK);
    snd_send(client);
}

SPICE_GNUC_VISIBLE void spice_server_record_set_mute(SpiceRecordInstance *sin, uint8_t mute)
{
    SpiceVolumeState *st = &sin->st->channel.volume;
    SndChannelClient *client = sin->st->channel.connection;

    st->mute = mute;

    if (!client)
        return;

    snd_set_command(client, SND_MUTE_MASK);
    snd_send(client);
}

static void snd_record_start(SndChannel *channel)
{
    SndChannelClient *client = channel->connection;

    channel->active = 1;
    if (!client) {
        return;
    }
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    spice_assert(!client->active);
    record_client->read_pos = record_client->write_pos = 0;   //todo: improve by
                                                              //stream generation
    client->active = TRUE;
    if (!client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_record_start(SpiceRecordInstance *sin)
{
    snd_record_start(&sin->st->channel);
}

SPICE_GNUC_VISIBLE void spice_server_record_stop(SpiceRecordInstance *sin)
{
    SndChannelClient *client = sin->st->channel.connection;

    sin->st->channel.active = 0;
    if (!client)
        return;
    spice_assert(client->active);
    client->active = FALSE;
    if (client->client_active) {
        snd_set_command(client, SND_CTRL_MASK);
        snd_send(client);
    } else {
        client->command &= ~SND_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_record_get_samples(SpiceRecordInstance *sin,
                                                            uint32_t *samples, uint32_t bufsize)
{
    SndChannelClient *client = sin->st->channel.connection;
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

    if (!client)
        return 0;
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(client);
    spice_assert(client->active);

    if (record_client->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

    len = MIN(record_client->write_pos - record_client->read_pos, bufsize);

    read_pos = record_client->read_pos % RECORD_SAMPLES_SIZE;
    record_client->read_pos += len;
    now = MIN(len, RECORD_SAMPLES_SIZE - read_pos);
    memcpy(samples, &record_client->samples[read_pos], now * 4);
    if (now < len) {
        memcpy(samples + now, record_client->samples, (len - now) * 4);
    }
    return len;
}

static uint32_t snd_get_best_rate(SndChannelClient *client, uint32_t cap_opus)
{
    int client_can_opus = TRUE;
    if (client) {
        client_can_opus = red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(client), cap_opus);
    }

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, SND_CODEC_ANY_FREQUENCY))
        return SND_CODEC_OPUS_PLAYBACK_FREQ;

    return SND_CODEC_CELT_PLAYBACK_FREQ;
}

static void snd_set_rate(SndChannel *channel, uint32_t frequency, uint32_t cap_opus)
{
    RedChannel *red_channel = RED_CHANNEL(channel);
    channel->frequency = frequency;
    if (red_channel && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency)) {
        red_channel_set_cap(red_channel, cap_opus);
    }
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_playback_rate(SpicePlaybackInstance *sin)
{
    return snd_get_best_rate(sin ? sin->st->channel.connection : NULL, SPICE_PLAYBACK_CAP_OPUS);
}

SPICE_GNUC_VISIBLE void spice_server_set_playback_rate(SpicePlaybackInstance *sin, uint32_t frequency)
{
    snd_set_rate(&sin->st->channel, frequency, SPICE_PLAYBACK_CAP_OPUS);
}

SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_record_rate(SpiceRecordInstance *sin)
{
    return snd_get_best_rate(sin ? sin->st->channel.connection : NULL, SPICE_RECORD_CAP_OPUS);
}

SPICE_GNUC_VISIBLE void spice_server_set_record_rate(SpiceRecordInstance *sin, uint32_t frequency)
{
    snd_set_rate(&sin->st->channel, frequency, SPICE_RECORD_CAP_OPUS);
}

static void on_new_record_channel_client(SndChannel *channel, SndChannelClient *client)
{
    spice_assert(client);

    channel->connection = client;
    if (channel->volume.volume_nchannels) {
        snd_set_command(client, SND_VOLUME_MUTE_MASK);
    }
    if (client->active) {
        snd_set_command(client, SND_CTRL_MASK);
    }
}

static void
record_channel_client_finalize(GObject *object)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(object);

    snd_codec_destroy(&record_client->codec);

    G_OBJECT_CLASS(record_channel_client_parent_class)->finalize(object);
}

static void
record_channel_client_constructed(GObject *object)
{
    RecordChannelClient *record_client = RECORD_CHANNEL_CLIENT(object);
    RedChannel *red_channel = red_channel_client_get_channel(RED_CHANNEL_CLIENT(record_client));
    SndChannel *channel = SND_CHANNEL(red_channel);

    G_OBJECT_CLASS(record_channel_client_parent_class)->constructed(object);

    on_new_record_channel_client(channel, SND_CHANNEL_CLIENT(record_client));
    if (channel->active) {
        snd_record_start(channel);
    }
    snd_send(SND_CHANNEL_CLIENT(record_client));
}


static void snd_set_record_peer(RedChannel *red_channel, RedClient *client, RedsStream *stream,
                                G_GNUC_UNUSED int migration,
                                int num_common_caps, uint32_t *common_caps,
                                int num_caps, uint32_t *caps)
{
    SndChannel *channel = SND_CHANNEL(red_channel);
    GArray *common_caps_array = NULL, *caps_array = NULL;
    RecordChannelClient *record_client;

    if (channel->connection) {
        red_channel_client_disconnect(RED_CHANNEL_CLIENT(channel->connection));
        channel->connection = NULL;
    }

    if (common_caps) {
        common_caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*common_caps),
                                              num_common_caps);
        g_array_append_vals(common_caps_array, common_caps, num_common_caps);
    }
    if (caps) {
        caps_array = g_array_sized_new(FALSE, FALSE, sizeof (*caps), num_caps);
        g_array_append_vals(caps_array, caps, num_caps);
    }

    record_client = g_initable_new(TYPE_RECORD_CHANNEL_CLIENT,
                                   NULL, NULL,
                                   "channel", channel,
                                   "client", client,
                                   "stream", stream,
                                   "caps", caps_array,
                                   "common-caps", common_caps_array,
                                   NULL);
    g_warn_if_fail(record_client != NULL);

    if (caps_array) {
        g_array_unref(caps_array);
    }
    if (common_caps_array) {
        g_array_unref(common_caps_array);
    }
}

static void snd_playback_migrate_channel_client(RedChannelClient *rcc)
{
    SndChannel *channel;
    RedChannel *red_channel = red_channel_client_get_channel(rcc);

    channel = SND_CHANNEL(red_channel);
    spice_assert(channel);
    spice_debug(NULL);

    if (channel->connection) {
        spice_assert(RED_CHANNEL_CLIENT(channel->connection) == rcc);
        snd_set_command(channel->connection, SND_MIGRATE_MASK);
        snd_send(channel->connection);
    }
}

static void add_channel(SndChannel *channel)
{
    channel->next = snd_channels;
    snd_channels = channel;
}

static void remove_channel(SndChannel *channel)
{
    SndChannel **now = &snd_channels;
    while (*now) {
        if (*now == channel) {
            *now = channel->next;
            return;
        }
        now = &(*now)->next;
    }
    spice_printerr("not found");
}

static void
snd_channel_init(SndChannel *self)
{
    self->frequency = SND_CODEC_CELT_PLAYBACK_FREQ; /* Default to the legacy rate */
}

static void
snd_channel_finalize(GObject *object)
{
    SndChannel *channel = SND_CHANNEL(object);

    remove_channel(channel);

    free(channel->volume.volume);
    channel->volume.volume = NULL;

    G_OBJECT_CLASS(snd_channel_parent_class)->finalize(object);
}

static void
snd_channel_class_init(SndChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->finalize = snd_channel_finalize;

    channel_class->config_socket = snd_channel_config_socket;
    channel_class->alloc_recv_buf = snd_channel_client_alloc_recv_buf;
    channel_class->release_recv_buf = snd_channel_client_release_recv_buf;
    channel_class->on_disconnect = snd_channel_on_disconnect;
}

static void
playback_channel_init(PlaybackChannel *self)
{
}

static void
playback_channel_constructed(GObject *object)
{
    ClientCbs client_cbs = { NULL, };
    SndChannel *self = SND_CHANNEL(object);
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));

    G_OBJECT_CLASS(playback_channel_parent_class)->constructed(object);

    client_cbs.connect = snd_set_playback_peer;
    client_cbs.disconnect = snd_disconnect_channel_client;
    client_cbs.migrate = snd_playback_migrate_channel_client;
    red_channel_register_client_cbs(RED_CHANNEL(self), &client_cbs, self);

    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY)) {
        red_channel_set_cap(RED_CHANNEL(self), SPICE_PLAYBACK_CAP_CELT_0_5_1);
    }
    red_channel_set_cap(RED_CHANNEL(self), SPICE_PLAYBACK_CAP_VOLUME);

    add_channel(self);
    reds_register_channel(reds, RED_CHANNEL(self));
}

static void
playback_channel_class_init(PlaybackChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = playback_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_PLAYBACK, NULL);
    channel_class->handle_parsed = playback_channel_handle_parsed;
    channel_class->send_item = playback_channel_send_item;
}

void snd_attach_playback(RedsState *reds, SpicePlaybackInstance *sin)
{
    sin->st = g_object_new(TYPE_PLAYBACK_CHANNEL,
                           "spice-server", reds,
                           "core-interface", reds_get_core_interface(reds),
                           "channel-type", SPICE_CHANNEL_PLAYBACK,
                           "id", 0,
                           NULL);
}

static void
record_channel_init(RecordChannel *self)
{
}

static void
record_channel_constructed(GObject *object)
{
    ClientCbs client_cbs = { NULL, };
    SndChannel *self = SND_CHANNEL(object);
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));

    G_OBJECT_CLASS(record_channel_parent_class)->constructed(object);

    client_cbs.connect = snd_set_record_peer;
    client_cbs.disconnect = snd_disconnect_channel_client;
    client_cbs.migrate = snd_record_migrate_channel_client;
    red_channel_register_client_cbs(RED_CHANNEL(self), &client_cbs, self);

    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY)) {
        red_channel_set_cap(RED_CHANNEL(self), SPICE_RECORD_CAP_CELT_0_5_1);
    }
    red_channel_set_cap(RED_CHANNEL(self), SPICE_RECORD_CAP_VOLUME);

    add_channel(self);
    reds_register_channel(reds, RED_CHANNEL(self));
}

static void
record_channel_class_init(RecordChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = record_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_RECORD, NULL);
    channel_class->handle_parsed = record_channel_handle_parsed;
    channel_class->send_item = record_channel_send_item;
}

void snd_attach_record(RedsState *reds, SpiceRecordInstance *sin)
{
    sin->st = g_object_new(TYPE_RECORD_CHANNEL,
                           "spice-server", reds,
                           "core-interface", reds_get_core_interface(reds),
                           "channel-type", SPICE_CHANNEL_RECORD,
                           "id", 0,
                           NULL);
}

static void snd_detach_common(SndChannel *channel)
{
    if (!channel) {
        return;
    }
    RedsState *reds = red_channel_get_server(RED_CHANNEL(channel));

    reds_unregister_channel(reds, RED_CHANNEL(channel));
    red_channel_destroy(RED_CHANNEL(channel));
}

void snd_detach_playback(SpicePlaybackInstance *sin)
{
    snd_detach_common(&sin->st->channel);
}

void snd_detach_record(SpiceRecordInstance *sin)
{
    snd_detach_common(&sin->st->channel);
}

void snd_set_playback_compression(int on)
{
    SndChannel *now = snd_channels;

    for (; now; now = now->next) {
        uint32_t type;
        g_object_get(RED_CHANNEL(now), "channel-type", &type, NULL);
        if (type == SPICE_CHANNEL_PLAYBACK && now->connection) {
            PlaybackChannelClient* playback = (PlaybackChannelClient*)now->connection;
            RedChannelClient *rcc = RED_CHANNEL_CLIENT(playback);
            int client_can_celt = red_channel_client_test_remote_cap(rcc,
                                    SPICE_PLAYBACK_CAP_CELT_0_5_1);
            int client_can_opus = red_channel_client_test_remote_cap(rcc,
                                    SPICE_PLAYBACK_CAP_OPUS);
            int desired_mode = snd_desired_audio_mode(on, now->frequency,
                                                      client_can_opus, client_can_celt);
            if (playback->mode != desired_mode) {
                playback->mode = desired_mode;
                snd_set_command(now->connection, SND_PLAYBACK_MODE_MASK);
            }
        }
    }
}

static void
snd_channel_client_class_init(SndChannelClientClass *self)
{
}

static void
snd_channel_client_init(SndChannelClient *self)
{
}

static void
playback_channel_client_class_init(PlaybackChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = playback_channel_client_constructed;
    object_class->finalize = playback_channel_client_finalize;
}

static void snd_playback_alloc_frames(PlaybackChannelClient *playback)
{
    int i;

    playback->frames = spice_new0(AudioFrameContainer, 1);
    playback->frames->refs = 1;
    for (i = 0; i < NUM_AUDIO_FRAMES; ++i) {
        playback->frames->items[i].container = playback->frames;
        snd_playback_free_frame(playback, &playback->frames->items[i]);
    }
}

static void
playback_channel_client_init(PlaybackChannelClient *playback)
{
    playback->mode = SPICE_AUDIO_DATA_MODE_RAW;
    snd_playback_alloc_frames(playback);
}

static void
record_channel_client_class_init(RecordChannelClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->constructed = record_channel_client_constructed;
    object_class->finalize = record_channel_client_finalize;
}

static void
record_channel_client_init(RecordChannelClient *record)
{
    record->mode = SPICE_AUDIO_DATA_MODE_RAW;
}
