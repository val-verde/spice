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

#include <fcntl.h>

#include "common-graphics-channel.h"
#include "dcc.h"
#include "red-client.h"

struct CommonGraphicsChannelPrivate
{
    int during_target_migrate; /* TRUE when the client that is associated with the channel
                                  is during migration. Turned off when the vm is started.
                                  The flag is used to avoid sending messages that are artifacts
                                  of the transition from stopped vm to loaded vm (e.g., recreation
                                  of the primary surface) */
};

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(CommonGraphicsChannel, common_graphics_channel,
                                    RED_TYPE_CHANNEL)

uint8_t *CommonGraphicsChannelClient::alloc_recv_buf(uint16_t type, uint32_t size)
{
    /* SPICE_MSGC_MIGRATE_DATA is the only client message whose size is dynamic */
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        return (uint8_t *) g_malloc(size);
    }

    if (size > sizeof(recv_buf)) {
        spice_warning("unexpected message size %u (max is %zd)", size, sizeof(recv_buf));
        return NULL;
    }
    return recv_buf;
}

void CommonGraphicsChannelClient::release_recv_buf(uint16_t type, uint32_t size, uint8_t* msg)
{
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        g_free(msg);
    }
}

bool CommonGraphicsChannelClient::config_socket()
{
    RedClient *client = get_client();
    MainChannelClient *mcc = red_client_get_main(client);
    RedStream *stream = get_stream();
    gboolean is_low_bandwidth;

    // TODO - this should be dynamic, not one time at channel creation
    is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    if (!red_stream_set_auto_flush(stream, false)) {
        /* FIXME: Using Nagle's Algorithm can lead to apparent delays, depending
         * on the delayed ack timeout on the other side.
         * Instead of using Nagle's, we need to implement message buffering on
         * the application level.
         * see: http://www.stuartcheshire.org/papers/NagleDelayedAck/
         */
        red_stream_set_no_delay(stream, !is_low_bandwidth);
    }
    // TODO: move wide/narrow ack setting to red_channel.
    ack_set_client_window(is_low_bandwidth ? WIDE_CLIENT_ACK_WINDOW : NARROW_CLIENT_ACK_WINDOW);
    return true;
}


static void
common_graphics_channel_class_init(CommonGraphicsChannelClass *klass)
{
}

static void
common_graphics_channel_init(CommonGraphicsChannel *self)
{
    self->priv = (CommonGraphicsChannelPrivate*) common_graphics_channel_get_instance_private(self);
}

void common_graphics_channel_set_during_target_migrate(CommonGraphicsChannel *self, gboolean value)
{
    self->priv->during_target_migrate = value;
}

gboolean common_graphics_channel_get_during_target_migrate(CommonGraphicsChannel *self)
{
    return self->priv->during_target_migrate;
}
