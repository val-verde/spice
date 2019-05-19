/*
   Copyright (C) 2009-2016 Red Hat, Inc.

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
#include <config.h>

#include <inttypes.h>
#include <common/generated_server_marshallers.h>

#include "main-channel-client.h"
#include "main-channel.h"
#include "red-channel-client.h"
#include "red-client.h"
#include "reds.h"

#define NET_TEST_WARMUP_BYTES 0
#define NET_TEST_BYTES (1024 * 250)

typedef enum {
    NET_TEST_STAGE_INVALID,
    NET_TEST_STAGE_WARMUP,
    NET_TEST_STAGE_LATENCY,
    NET_TEST_STAGE_RATE,
    NET_TEST_STAGE_COMPLETE,
} NetTestStage;

#define CLIENT_CONNECTIVITY_TIMEOUT (MSEC_PER_SEC * 30)

// approximate max receive message size for main channel
#define MAIN_CHANNEL_RECEIVE_BUF_SIZE \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

struct MainChannelClientPrivate {
    SPICE_CXX_GLIB_ALLOCATOR

    uint32_t connection_id;
    uint32_t ping_id = 0;
    uint32_t net_test_id = 0;
    NetTestStage net_test_stage = NET_TEST_STAGE_INVALID;
    uint64_t latency = 0;
    uint64_t bitrate_per_sec = ~0;
    int mig_wait_connect = 0;
    int mig_connect_ok = 0;
    int mig_wait_prev_complete = 0;
    int mig_wait_prev_try_seamless = 0;
    int init_sent = 0;
    int seamless_mig_dst = 0;
    bool initial_channels_list_sent = false;
    uint8_t recv_buf[MAIN_CHANNEL_RECEIVE_BUF_SIZE];
};

typedef struct RedPingPipeItem {
    RedPipeItem base;
    int size;
} RedPingPipeItem;

typedef struct RedTokensPipeItem {
    RedPipeItem base;
    int tokens;
} RedTokensPipeItem;

typedef struct RedAgentDataPipeItem {
    RedPipeItem base;
    uint8_t* data;
    size_t len;
    spice_marshaller_item_free_func free_data;
    void *opaque;
} RedAgentDataPipeItem;

typedef struct RedInitPipeItem {
    RedPipeItem base;
    int connection_id;
    int display_channels_hint;
    int current_mouse_mode;
    int is_client_mouse_allowed;
    int multi_media_time;
    int ram_hint;
} RedInitPipeItem;

typedef struct RedNamePipeItem {
    RedPipeItem base;
    SpiceMsgMainName msg;
} RedNamePipeItem;

typedef struct RedUuidPipeItem {
    RedPipeItem base;
    SpiceMsgMainUuid msg;
} RedUuidPipeItem;

typedef struct RedNotifyPipeItem {
    RedPipeItem base;
    char *msg;
} RedNotifyPipeItem;

typedef struct RedMouseModePipeItem {
    RedPipeItem base;
    SpiceMouseMode current_mode;
    int is_client_mouse_allowed;
} RedMouseModePipeItem;

typedef struct RedMultiMediaTimePipeItem {
    RedPipeItem base;
    uint32_t time;
} RedMultiMediaTimePipeItem;

typedef struct RedRegisteredChannelPipeItem {
    RedPipeItem base;
    uint32_t channel_type;
    uint32_t channel_id;
} RedRegisteredChannelPipeItem;

#define ZERO_BUF_SIZE 4096

static const uint8_t zero_page[ZERO_BUF_SIZE] = {0};

uint8_t *MainChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        RedChannel *channel = get_channel();
        return reds_get_agent_data_buffer(channel->get_server(), this, size);
    } else if (size > sizeof(priv->recv_buf)) {
        /* message too large, caller will log a message and close the connection */
        return NULL;
    } else {
        return priv->recv_buf;
    }
}

void MainChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t *msg)
{
    if (type == SPICE_MSGC_MAIN_AGENT_DATA) {
        RedChannel *channel = get_channel();
        reds_release_agent_data_buffer(channel->get_server(), msg);
    }
}

/*
 * When the main channel is disconnected, disconnect the entire client.
 */
void MainChannelClient::on_disconnect()
{
    RedsState *reds = get_channel()->get_server();
    main_dispatcher_client_disconnect(reds_get_main_dispatcher(reds),
                                      get_client());
}

static void main_channel_client_push_ping(MainChannelClient *mcc, int size);

static void main_notify_item_free(RedPipeItem *base)
{
    RedNotifyPipeItem *data = SPICE_UPCAST(RedNotifyPipeItem, base);
    g_free(data->msg);
    g_free(data);
}

static RedPipeItem *main_notify_item_new(const char *msg, int num)
{
    RedNotifyPipeItem *item = g_new(RedNotifyPipeItem, 1);

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_MAIN_NOTIFY,
                            main_notify_item_free);
    item->msg = g_strdup(msg);
    return &item->base;
}

void main_channel_client_start_net_test(MainChannelClient *mcc, int test_rate)
{
    if (!mcc || mcc->priv->net_test_id) {
        return;
    }

    if (!test_rate) {
        mcc->start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
        return;
    }

    mcc->priv->net_test_id = mcc->priv->ping_id + 1;
    mcc->priv->net_test_stage = NET_TEST_STAGE_WARMUP;

    main_channel_client_push_ping(mcc, NET_TEST_WARMUP_BYTES);
    main_channel_client_push_ping(mcc, 0);
    main_channel_client_push_ping(mcc, NET_TEST_BYTES);
}

static RedPipeItem *red_ping_item_new(int size)
{
    RedPingPipeItem *item = g_new(RedPingPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_PING);
    item->size = size;
    return &item->base;
}

static void main_channel_client_push_ping(MainChannelClient *mcc, int size)
{
    RedPipeItem *item = red_ping_item_new(size);
    mcc->pipe_add_push(item);
}

static RedPipeItem *main_agent_tokens_item_new(uint32_t num_tokens)
{
    RedTokensPipeItem *item = g_new(RedTokensPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN);
    item->tokens = num_tokens;
    return &item->base;
}


void main_channel_client_push_agent_tokens(MainChannelClient *mcc, uint32_t num_tokens)
{
    RedPipeItem *item = main_agent_tokens_item_new(num_tokens);

    mcc->pipe_add_push(item);
}

static void main_agent_data_item_free(RedPipeItem *base)
{
    RedAgentDataPipeItem *item = SPICE_UPCAST(RedAgentDataPipeItem, base);
    item->free_data(item->data, item->opaque);
    g_free(item);
}

static RedPipeItem *main_agent_data_item_new(uint8_t* data, size_t len,
                                             spice_marshaller_item_free_func free_data,
                                             void *opaque)
{
    RedAgentDataPipeItem *item = g_new(RedAgentDataPipeItem, 1);

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA,
                            main_agent_data_item_free);
    item->data = data;
    item->len = len;
    item->free_data = free_data;
    item->opaque = opaque;
    return &item->base;
}

void main_channel_client_push_agent_data(MainChannelClient *mcc, uint8_t* data, size_t len,
           spice_marshaller_item_free_func free_data, void *opaque)
{
    RedPipeItem *item;

    item = main_agent_data_item_new(data, len, free_data, opaque);
    mcc->pipe_add_push(item);
}

static RedPipeItem *main_init_item_new(int connection_id,
                                       int display_channels_hint,
                                       SpiceMouseMode current_mouse_mode,
                                       int is_client_mouse_allowed,
                                       int multi_media_time,
                                       int ram_hint)
{
    RedInitPipeItem *item = g_new(RedInitPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_INIT);
    item->connection_id = connection_id;
    item->display_channels_hint = display_channels_hint;
    item->current_mouse_mode = current_mouse_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    item->multi_media_time = multi_media_time;
    item->ram_hint = ram_hint;
    return &item->base;
}

void main_channel_client_push_init(MainChannelClient *mcc,
                                   int display_channels_hint,
                                   SpiceMouseMode current_mouse_mode,
                                   int is_client_mouse_allowed,
                                   int multi_media_time,
                                   int ram_hint)
{
    RedPipeItem *item;

    item = main_init_item_new(mcc->priv->connection_id, display_channels_hint,
                              current_mouse_mode, is_client_mouse_allowed,
                              multi_media_time, ram_hint);
    mcc->pipe_add_push(item);
}

static RedPipeItem *main_name_item_new(const char *name)
{
    RedNamePipeItem *item = (RedNamePipeItem*) g_malloc(sizeof(RedNamePipeItem) + strlen(name) + 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_NAME);
    item->msg.name_len = strlen(name) + 1;
    memcpy(&item->msg.name, name, item->msg.name_len);

    return &item->base;
}

void main_channel_client_push_name(MainChannelClient *mcc, const char *name)
{
    RedPipeItem *item;

    if (!mcc->test_remote_cap(SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    item = main_name_item_new(name);
    mcc->pipe_add_push(item);
}

static RedPipeItem *main_uuid_item_new(const uint8_t uuid[16])
{
    RedUuidPipeItem *item = g_new(RedUuidPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_UUID);
    memcpy(item->msg.uuid, uuid, sizeof(item->msg.uuid));

    return &item->base;
}

void main_channel_client_push_uuid(MainChannelClient *mcc, const uint8_t uuid[16])
{
    RedPipeItem *item;

    if (!mcc->test_remote_cap(SPICE_MAIN_CAP_NAME_AND_UUID))
        return;

    item = main_uuid_item_new(uuid);
    mcc->pipe_add_push(item);
}

void main_channel_client_push_notify(MainChannelClient *mcc, const char *msg)
{
    RedPipeItem *item = main_notify_item_new(msg, 1);
    mcc->pipe_add_push(item);
}

RedPipeItem *main_mouse_mode_item_new(SpiceMouseMode current_mode, int is_client_mouse_allowed)
{
    RedMouseModePipeItem *item = g_new(RedMouseModePipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE);
    item->current_mode = current_mode;
    item->is_client_mouse_allowed = is_client_mouse_allowed;
    return &item->base;
}

RedPipeItem *main_multi_media_time_item_new(uint32_t mm_time)
{
    RedMultiMediaTimePipeItem *item;

    item = g_new(RedMultiMediaTimePipeItem, 1);
    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME);
    item->time = mm_time;
    return &item->base;
}

RedPipeItem *registered_channel_item_new(RedChannel *channel)
{
    RedRegisteredChannelPipeItem *item;

    item = g_new0(RedRegisteredChannelPipeItem, 1);
    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_MAIN_REGISTERED_CHANNEL);

    uint32_t type, id;
    g_object_get(channel, "channel-type", &type, "id", &id, NULL);
    item->channel_type = type;
    item->channel_id = id;
    return &item->base;
}

void main_channel_client_handle_migrate_connected(MainChannelClient *mcc,
                                                  int success,
                                                  int seamless)
{
    if (mcc->priv->mig_wait_connect) {
        RedChannel *channel = mcc->get_channel();
        MainChannel *main_channel = MAIN_CHANNEL(channel);

        mcc->priv->mig_wait_connect = FALSE;
        mcc->priv->mig_connect_ok = success;
        main_channel_on_migrate_connected(main_channel, success, seamless);
    } else {
        if (success) {
            mcc->pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    }
}

void main_channel_client_handle_migrate_dst_do_seamless(MainChannelClient *mcc,
                                                        uint32_t src_version)
{
    RedChannel *channel = mcc->get_channel();
    if (reds_on_migrate_dst_set_seamless(channel->get_server(), mcc, src_version)) {
        mcc->priv->seamless_mig_dst = TRUE;
        mcc->pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_ACK);
    } else {
        mcc->pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_DST_SEAMLESS_NACK);
    }
}
void main_channel_client_handle_pong(MainChannelClient *mcc, SpiceMsgPing *ping, uint32_t size)
{
    uint64_t roundtrip;

    roundtrip = spice_get_monotonic_time_ns() / NSEC_PER_MICROSEC - ping->timestamp;

    if (ping->id != mcc->priv->net_test_id) {
        /*
         * channel client monitors the connectivity using ping-pong messages
         */
        RedChannelClient::handle_message(mcc, SPICE_MSGC_PONG, size, ping);
        return;
    }

    switch (mcc->priv->net_test_stage) {
    case NET_TEST_STAGE_WARMUP:
        mcc->priv->net_test_id++;
        mcc->priv->net_test_stage = NET_TEST_STAGE_LATENCY;
        mcc->priv->latency = roundtrip;
        break;
    case NET_TEST_STAGE_LATENCY:
        mcc->priv->net_test_id++;
        mcc->priv->net_test_stage = NET_TEST_STAGE_RATE;
        mcc->priv->latency = MIN(mcc->priv->latency, roundtrip);
        break;
    case NET_TEST_STAGE_RATE:
        mcc->priv->net_test_id = 0;
        if (roundtrip <= mcc->priv->latency) {
            // probably high load on client or server result with incorrect values
            red_channel_debug(mcc->get_channel(),
                              "net test: invalid values, latency %" G_GUINT64_FORMAT
                              " roundtrip %" G_GUINT64_FORMAT ". assuming high"
                              "bandwidth", mcc->priv->latency, roundtrip);
            mcc->priv->latency = 0;
            mcc->priv->net_test_stage = NET_TEST_STAGE_INVALID;
            mcc->start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
            break;
        }
        mcc->priv->bitrate_per_sec = (uint64_t)(NET_TEST_BYTES * 8) * 1000000
            / (roundtrip - mcc->priv->latency);
        mcc->priv->net_test_stage = NET_TEST_STAGE_COMPLETE;
        red_channel_debug(mcc->get_channel(),
                          "net test: latency %f ms, bitrate %" G_GUINT64_FORMAT " bps (%f Mbps)%s",
                          (double)mcc->priv->latency / 1000,
                          mcc->priv->bitrate_per_sec,
                          (double)mcc->priv->bitrate_per_sec / 1024 / 1024,
                          main_channel_client_is_low_bandwidth(mcc) ? " LOW BANDWIDTH" : "");
        mcc->start_connectivity_monitoring(CLIENT_CONNECTIVITY_TIMEOUT);
        break;
    default:
        red_channel_warning(mcc->get_channel(),
                            "invalid net test stage, ping id %d test id %d stage %d",
                            ping->id,
                            mcc->priv->net_test_id,
                            mcc->priv->net_test_stage);
        mcc->priv->net_test_stage = NET_TEST_STAGE_INVALID;
    }
}

void main_channel_client_handle_migrate_end(MainChannelClient *mcc)
{
    RedClient *client = mcc->get_client();
    if (!red_client_during_migrate_at_target(client)) {
        red_channel_warning(mcc->get_channel(),
                            "unexpected SPICE_MSGC_MIGRATE_END");
        return;
    }
    if (!mcc->test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        red_channel_warning(mcc->get_channel(),
                            "unexpected SPICE_MSGC_MIGRATE_END, "
                            "client does not support semi-seamless migration");
        return;
    }
    red_client_semi_seamless_migrate_complete(client);
}

void main_channel_client_migrate_cancel_wait(MainChannelClient *mcc)
{
    if (mcc->priv->mig_wait_connect) {
        mcc->priv->mig_wait_connect = FALSE;
        mcc->priv->mig_connect_ok = FALSE;
    }
    mcc->priv->mig_wait_prev_complete = FALSE;
}

void main_channel_client_migrate_dst_complete(MainChannelClient *mcc)
{
    if (mcc->priv->mig_wait_prev_complete) {
        if (mcc->priv->mig_wait_prev_try_seamless) {
            RedChannel *channel = mcc->get_channel();
            spice_assert(channel->get_n_clients() == 1);
            mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        } else {
            mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
        }
        mcc->priv->mig_wait_connect = TRUE;
        mcc->priv->mig_wait_prev_complete = FALSE;
    }
}

gboolean main_channel_client_migrate_src_complete(MainChannelClient *mcc,
                                                  gboolean success)
{
    gboolean ret = FALSE;
    bool semi_seamless_support = mcc->test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE);
    if (semi_seamless_support && mcc->priv->mig_connect_ok) {
        if (success) {
            mcc->pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_END);
            ret = TRUE;
        } else {
            mcc->pipe_add_empty_msg(SPICE_MSG_MAIN_MIGRATE_CANCEL);
        }
    } else {
        if (success) {
            mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST);
        }
    }
    mcc->priv->mig_connect_ok = FALSE;
    mcc->priv->mig_wait_connect = FALSE;

    return ret;
}


MainChannelClient::MainChannelClient(MainChannel *channel,
                                     RedClient *client,
                                     RedStream *stream,
                                     RedChannelCapabilities *caps,
                                     uint32_t connection_id):
    RedChannelClient(RED_CHANNEL(channel), client, stream, caps),
    priv(new MainChannelClientPrivate())
{
    priv->connection_id = connection_id;
}

MainChannelClient::~MainChannelClient()
{
    delete priv;
}

MainChannelClient *main_channel_client_create(MainChannel *main_chan, RedClient *client,
                                              RedStream *stream, uint32_t connection_id,
                                              RedChannelCapabilities *caps)
{
    auto mcc = new MainChannelClient(main_chan, client, stream, caps, connection_id);
    if (!mcc->init()) {
        mcc->unref();
        mcc = nullptr;
    }
    return mcc;
}

int main_channel_client_is_network_info_initialized(MainChannelClient *mcc)
{
    return mcc->priv->net_test_stage == NET_TEST_STAGE_COMPLETE;
}

int main_channel_client_is_low_bandwidth(MainChannelClient *mcc)
{
    // TODO: configurable?
    return mcc->priv->bitrate_per_sec < 10 * 1024 * 1024;
}

uint64_t main_channel_client_get_bitrate_per_sec(MainChannelClient *mcc)
{
    return mcc->priv->bitrate_per_sec;
}

uint64_t main_channel_client_get_roundtrip_ms(MainChannelClient *mcc)
{
    return mcc->priv->latency / 1000;
}

XXX_CAST(RedChannelClient, MainChannelClient, MAIN_CHANNEL_CLIENT);

void main_channel_client_migrate(RedChannelClient *rcc)
{
    RedChannel *channel = rcc->get_channel();
    reds_on_main_channel_migrate(channel->get_server(),
                                 MAIN_CHANNEL_CLIENT(rcc));
    RedChannelClient::default_migrate(rcc);
}

gboolean main_channel_client_connect_semi_seamless(MainChannelClient *mcc)
{
    if (mcc->test_remote_cap(SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE)) {
        RedClient *client = mcc->get_client();
        if (red_client_during_migrate_at_target(client)) {
            mcc->priv->mig_wait_prev_complete = TRUE;
            mcc->priv->mig_wait_prev_try_seamless = FALSE;
        } else {
            mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN);
            mcc->priv->mig_wait_connect = TRUE;
        }
        mcc->priv->mig_connect_ok = FALSE;
        return TRUE;
    }
    return FALSE;
}

void main_channel_client_connect_seamless(MainChannelClient *mcc)
{
    RedClient *client = mcc->get_client();
    spice_assert(mcc->test_remote_cap(SPICE_MAIN_CAP_SEAMLESS_MIGRATE));
    if (red_client_during_migrate_at_target(client)) {
        mcc->priv->mig_wait_prev_complete = TRUE;
        mcc->priv->mig_wait_prev_try_seamless = TRUE;
    } else {
        mcc->pipe_add_type(RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS);
        mcc->priv->mig_wait_connect = TRUE;
    }
    mcc->priv->mig_connect_ok = FALSE;
}

uint32_t main_channel_client_get_connection_id(MainChannelClient *mcc)
{
    return mcc->priv->connection_id;
}

static uint32_t main_channel_client_next_ping_id(MainChannelClient *mcc)
{
    return ++mcc->priv->ping_id;
}

static void main_channel_marshall_channels(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           RedPipeItem *item)
{
    SpiceMsgChannels* channels_info;
    RedChannel *channel = rcc->get_channel();

    rcc->init_send_data(SPICE_MSG_MAIN_CHANNELS_LIST);
    channels_info = reds_msg_channels_new(channel->get_server());
    spice_marshall_msg_main_channels_list(m, channels_info);
    g_free(channels_info);
}

static void main_channel_marshall_ping(RedChannelClient *rcc,
                                       SpiceMarshaller *m,
                                       RedPingPipeItem *item)
{
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
    SpiceMsgPing ping;
    int size_left = item->size;

    rcc->init_send_data(SPICE_MSG_PING);
    ping.id = main_channel_client_next_ping_id(mcc);
    ping.timestamp = spice_get_monotonic_time_ns() / NSEC_PER_MICROSEC;
    spice_marshall_msg_ping(m, &ping);

    while (size_left > 0) {
        int now = MIN(ZERO_BUF_SIZE, size_left);
        size_left -= now;
        spice_marshaller_add_by_ref(m, zero_page, now);
    }
}

static void main_channel_marshall_mouse_mode(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedMouseModePipeItem *item)
{
    SpiceMsgMainMouseMode mouse_mode;

    rcc->init_send_data(SPICE_MSG_MAIN_MOUSE_MODE);
    mouse_mode.supported_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        mouse_mode.supported_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    mouse_mode.current_mode = item->current_mode;
    spice_marshall_msg_main_mouse_mode(m, &mouse_mode);
}

static void main_channel_marshall_agent_disconnected(RedChannelClient *rcc,
                                                     SpiceMarshaller *m,
                                                     RedPipeItem *item)
{
    SpiceMsgMainAgentDisconnect disconnect;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_DISCONNECTED);
    disconnect.error_code = SPICE_LINK_ERR_OK;
    spice_marshall_msg_main_agent_disconnected(m, &disconnect);
}

static void main_channel_marshall_tokens(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedTokensPipeItem *item)
{
    SpiceMsgMainAgentTokens tokens;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_TOKEN);
    tokens.num_tokens = item->tokens;
    spice_marshall_msg_main_agent_token(m, &tokens);
}

static void main_channel_marshall_agent_data(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedAgentDataPipeItem *item)
{
    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_DATA);
    /* since pipe item owns the data, keep it alive until it's sent */
    red_pipe_item_ref(&item->base);
    spice_marshaller_add_by_ref_full(m, item->data, item->len, marshaller_unref_pipe_item, item);
}

static void main_channel_marshall_migrate_data_item(RedChannelClient *rcc,
                                                    SpiceMarshaller *m,
                                                    RedPipeItem *item)
{
    RedChannel *channel = rcc->get_channel();
    rcc->init_send_data(SPICE_MSG_MIGRATE_DATA);
    // TODO: from reds split. ugly separation.
    reds_marshall_migrate_data(channel->get_server(), m);
}

static void main_channel_marshall_init(RedChannelClient *rcc,
                                       SpiceMarshaller *m,
                                       RedInitPipeItem *item)
{
    SpiceMsgMainInit init; // TODO - remove this copy, make RedInitPipeItem reuse SpiceMsgMainInit
    RedChannel *channel = rcc->get_channel();

    rcc->init_send_data(SPICE_MSG_MAIN_INIT);
    init.session_id = item->connection_id;
    init.display_channels_hint = item->display_channels_hint;
    init.current_mouse_mode = item->current_mouse_mode;
    init.supported_mouse_modes = SPICE_MOUSE_MODE_SERVER;
    if (item->is_client_mouse_allowed) {
        init.supported_mouse_modes |= SPICE_MOUSE_MODE_CLIENT;
    }
    init.agent_connected = reds_has_vdagent(channel->get_server());
    init.agent_tokens = REDS_AGENT_WINDOW_SIZE;
    init.multi_media_time = item->multi_media_time;
    init.ram_hint = item->ram_hint;
    spice_marshall_msg_main_init(m, &init);
}

static void main_channel_marshall_notify(RedChannelClient *rcc,
                                         SpiceMarshaller *m, RedNotifyPipeItem *item)
{
    SpiceMsgNotify notify;

    rcc->init_send_data(SPICE_MSG_NOTIFY);
    notify.time_stamp = spice_get_monotonic_time_ns(); // TODO - move to main_new_notify_item
    notify.severity = SPICE_NOTIFY_SEVERITY_WARN;
    notify.visibilty = SPICE_NOTIFY_VISIBILITY_HIGH;
    notify.what = SPICE_WARN_GENERAL;
    notify.message_len = strlen(item->msg);
    spice_marshall_msg_notify(m, &notify);
    spice_marshaller_add(m, (uint8_t *)item->msg, notify.message_len + 1);
}

static void main_channel_fill_migrate_dst_info(MainChannel *main_channel,
                                               SpiceMigrationDstInfo *dst_info)
{
    const RedsMigSpice *mig_dst = main_channel_get_migration_target(main_channel);
    dst_info->port = mig_dst->port;
    dst_info->sport = mig_dst->sport;
    dst_info->host_size = strlen(mig_dst->host) + 1;
    dst_info->host_data = (uint8_t *)mig_dst->host;
    if (mig_dst->cert_subject) {
        dst_info->cert_subject_size = strlen(mig_dst->cert_subject) + 1;
        dst_info->cert_subject_data = (uint8_t *)mig_dst->cert_subject;
    } else {
        dst_info->cert_subject_size = 0;
        dst_info->cert_subject_data = NULL;
    }
}

static void main_channel_marshall_migrate_begin(SpiceMarshaller *m, RedChannelClient *rcc,
                                                RedPipeItem *item)
{
    RedChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrationBegin migrate;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_BEGIN);
    main_channel_fill_migrate_dst_info(MAIN_CHANNEL(channel), &migrate.dst_info);
    spice_marshall_msg_main_migrate_begin(m, &migrate);
}

static void main_channel_marshall_migrate_begin_seamless(SpiceMarshaller *m,
                                                         RedChannelClient *rcc,
                                                         RedPipeItem *item)
{
    RedChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrateBeginSeamless migrate_seamless;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_BEGIN_SEAMLESS);
    main_channel_fill_migrate_dst_info(MAIN_CHANNEL(channel), &migrate_seamless.dst_info);
    migrate_seamless.src_mig_version = SPICE_MIGRATION_PROTOCOL_VERSION;
    spice_marshall_msg_main_migrate_begin_seamless(m, &migrate_seamless);
}

static void main_channel_marshall_multi_media_time(RedChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   RedMultiMediaTimePipeItem *item)
{
    SpiceMsgMainMultiMediaTime time_mes;

    rcc->init_send_data(SPICE_MSG_MAIN_MULTI_MEDIA_TIME);
    time_mes.time = item->time;
    spice_marshall_msg_main_multi_media_time(m, &time_mes);
}

static void main_channel_marshall_migrate_switch(SpiceMarshaller *m, RedChannelClient *rcc,
                                                 RedPipeItem *item)
{
    RedChannel *channel = rcc->get_channel();
    SpiceMsgMainMigrationSwitchHost migrate;
    MainChannel *main_ch;
    const RedsMigSpice *mig_target;

    rcc->init_send_data(SPICE_MSG_MAIN_MIGRATE_SWITCH_HOST);
    main_ch = MAIN_CHANNEL(channel);
    mig_target = main_channel_get_migration_target(main_ch);
    migrate.port = mig_target->port;
    migrate.sport = mig_target->sport;
    migrate.host_size = strlen(mig_target->host) + 1;
    migrate.host_data = (uint8_t *)mig_target->host;
    if (mig_target->cert_subject) {
        migrate.cert_subject_size = strlen(mig_target->cert_subject) + 1;
        migrate.cert_subject_data = (uint8_t *)mig_target->cert_subject;
    } else {
        migrate.cert_subject_size = 0;
        migrate.cert_subject_data = NULL;
    }
    spice_marshall_msg_main_migrate_switch_host(m, &migrate);
}

static void main_channel_marshall_agent_connected(SpiceMarshaller *m,
                                                  RedChannelClient *rcc,
                                                  RedPipeItem *item)
{
    SpiceMsgMainAgentConnectedTokens connected;

    rcc->init_send_data(SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS);
    connected.num_tokens = REDS_AGENT_WINDOW_SIZE;
    spice_marshall_msg_main_agent_connected_tokens(m, &connected);
}

static void main_channel_marshall_registered_channel(RedChannelClient *rcc,
                                                     SpiceMarshaller *m,
                                                     RedRegisteredChannelPipeItem *item)
{
    struct {
        SpiceMsgChannels info;
        SpiceChannelId ids[1];
    } channels_info_buffer;
    SpiceMsgChannels* channels_info = &channels_info_buffer.info;

    rcc->init_send_data(SPICE_MSG_MAIN_CHANNELS_LIST);

    channels_info->channels[0].type = item->channel_type;
    channels_info->channels[0].id = item->channel_id;
    channels_info->num_of_channels = 1;

    spice_marshall_msg_main_channels_list(m, channels_info);
}

void main_channel_client_send_item(RedChannelClient *rcc, RedPipeItem *base)
{
    MainChannelClient *mcc = MAIN_CHANNEL_CLIENT(rcc);
    SpiceMarshaller *m = rcc->get_marshaller();

    /* In semi-seamless migration (dest side), the connection is started from scratch, and
     * we ignore any pipe item that arrives before the INIT msg is sent.
     * For seamless we don't send INIT, and the connection continues from the same place
     * it stopped on the src side. */
    if (!mcc->priv->init_sent &&
        !mcc->priv->seamless_mig_dst &&
        base->type != RED_PIPE_ITEM_TYPE_MAIN_INIT) {
        red_channel_warning(rcc->get_channel(),
                            "Init msg for client %p was not sent yet "
                            "(client is probably during semi-seamless migration). Ignoring msg type %d",
                            rcc->get_client(), base->type);
        return;
    }
    switch (base->type) {
        case RED_PIPE_ITEM_TYPE_MAIN_CHANNELS_LIST:
            main_channel_marshall_channels(rcc, m, base);
            mcc->priv->initial_channels_list_sent = true;
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_PING:
            main_channel_marshall_ping(rcc, m,
                SPICE_UPCAST(RedPingPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MOUSE_MODE:
            main_channel_marshall_mouse_mode(rcc, m,
                SPICE_UPCAST(RedMouseModePipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DISCONNECTED:
            main_channel_marshall_agent_disconnected(rcc, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_TOKEN:
            main_channel_marshall_tokens(rcc, m,
                SPICE_UPCAST(RedTokensPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_DATA:
            main_channel_marshall_agent_data(rcc, m,
                SPICE_UPCAST(RedAgentDataPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_DATA:
            main_channel_marshall_migrate_data_item(rcc, m, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_INIT:
            mcc->priv->init_sent = TRUE;
            main_channel_marshall_init(rcc, m,
                SPICE_UPCAST(RedInitPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NOTIFY:
            main_channel_marshall_notify(rcc, m,
                SPICE_UPCAST(RedNotifyPipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN:
            main_channel_marshall_migrate_begin(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_BEGIN_SEAMLESS:
            main_channel_marshall_migrate_begin_seamless(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MULTI_MEDIA_TIME:
            main_channel_marshall_multi_media_time(rcc, m,
                SPICE_UPCAST(RedMultiMediaTimePipeItem, base));
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_MIGRATE_SWITCH_HOST:
            main_channel_marshall_migrate_switch(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_NAME:
            rcc->init_send_data(SPICE_MSG_MAIN_NAME);
            spice_marshall_msg_main_name(m, &SPICE_UPCAST(RedNamePipeItem, base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_UUID:
            rcc->init_send_data(SPICE_MSG_MAIN_UUID);
            spice_marshall_msg_main_uuid(m, &SPICE_UPCAST(RedUuidPipeItem, base)->msg);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_AGENT_CONNECTED_TOKENS:
            main_channel_marshall_agent_connected(m, rcc, base);
            break;
        case RED_PIPE_ITEM_TYPE_MAIN_REGISTERED_CHANNEL:
            /* The spice protocol requires that the server receive a ATTACH_CHANNELS
             * message from the client before sending any CHANNEL_LIST message. If
             * we've already sent our initial CHANNELS_LIST message, then it should be
             * safe to send new ones for newly-registered channels. */
            if (!mcc->priv->initial_channels_list_sent) {
                return;
            }
            main_channel_marshall_registered_channel(rcc, m,
                SPICE_UPCAST(RedRegisteredChannelPipeItem, base));
            break;
        default:
            break;
    };
    rcc->begin_send_message();
}
