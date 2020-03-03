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
#include <config.h>

#include <glib.h>
#include <common/generated_server_marshallers.h>

#include "common-graphics-channel.h"
#include "cursor-channel.h"
#include "cursor-channel-client.h"
#include "reds.h"

typedef struct RedCursorPipeItem {
    RedPipeItem base;
    RedCursorCmd *red_cursor;
} RedCursorPipeItem;

static void cursor_pipe_item_free(RedPipeItem *pipe_item);

static RedCursorPipeItem *cursor_pipe_item_new(RedCursorCmd *cmd)
{
    RedCursorPipeItem *item = g_new0(RedCursorPipeItem, 1);

    spice_return_val_if_fail(cmd != NULL, NULL);

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_CURSOR,
                            cursor_pipe_item_free);
    item->red_cursor = red_cursor_cmd_ref(cmd);

    return item;
}

static void cursor_pipe_item_free(RedPipeItem *base)
{
    RedCursorPipeItem *pipe_item = SPICE_UPCAST(RedCursorPipeItem, base);

    red_cursor_cmd_unref(pipe_item->red_cursor);
    g_free(pipe_item);
}

static void cursor_channel_set_item(CursorChannel *cursor, RedCursorPipeItem *item)
{
    if (item) {
        red_pipe_item_ref(&item->base);
    }
    if (cursor->item) {
        red_pipe_item_unref(&cursor->item->base);
    }
    cursor->item = item;
}

static void cursor_fill(CursorChannelClient *ccc, RedCursorPipeItem *cursor,
                        SpiceCursor *red_cursor, SpiceMarshaller *m)
{
    RedCursorCmd *cursor_cmd;

    if (!cursor) {
        red_cursor->flags = SPICE_CURSOR_FLAGS_NONE;
        return;
    }

    cursor_cmd = cursor->red_cursor;
    *red_cursor = cursor_cmd->u.set.shape;

    if (red_cursor->header.unique) {
        if (ccc->cache_find(red_cursor->header.unique)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_FROM_CACHE;
            return;
        }
        if (ccc->cache_add(red_cursor->header.unique, 1)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_CACHE_ME;
        }
    }

    if (red_cursor->data_size) {
        SpiceMarshaller *m2 = spice_marshaller_get_submarshaller(m);
        red_pipe_item_ref(&cursor->base);
        spice_marshaller_add_by_ref_full(m2, red_cursor->data, red_cursor->data_size,
                                         marshaller_unref_pipe_item, &cursor->base);
    }
}

static void red_marshall_cursor_init(CursorChannelClient *ccc, SpiceMarshaller *base_marshaller)
{
    spice_assert(ccc);

    CursorChannel *cursor_channel;
    SpiceMsgCursorInit msg;

    cursor_channel = ccc->get_channel();

    ccc->init_send_data(SPICE_MSG_CURSOR_INIT);
    msg.visible = cursor_channel->cursor_visible;
    msg.position = cursor_channel->cursor_position;
    msg.trail_length = cursor_channel->cursor_trail_length;
    msg.trail_frequency = cursor_channel->cursor_trail_frequency;

    cursor_fill(ccc, cursor_channel->item, &msg.cursor, base_marshaller);
    spice_marshall_msg_cursor_init(base_marshaller, &msg);
}

static void red_marshall_cursor(CursorChannelClient *ccc,
                                SpiceMarshaller *m,
                                RedCursorPipeItem *cursor_pipe_item)
{
    CursorChannel *cursor_channel = ccc->get_channel();
    RedCursorPipeItem *item = cursor_pipe_item;
    RedCursorCmd *cmd;

    spice_return_if_fail(cursor_channel);

    cmd = item->red_cursor;
    switch (cmd->type) {
    case QXL_CURSOR_MOVE:
        {
            SpiceMsgCursorMove cursor_move;
            ccc->init_send_data(SPICE_MSG_CURSOR_MOVE);
            cursor_move.position = cmd->u.position;
            spice_marshall_msg_cursor_move(m, &cursor_move);
            break;
        }
    case QXL_CURSOR_SET:
        {
            SpiceMsgCursorSet cursor_set;

            ccc->init_send_data(SPICE_MSG_CURSOR_SET);
            cursor_set.position = cmd->u.set.position;
            cursor_set.visible = cursor_channel->cursor_visible;

            cursor_fill(ccc, item, &cursor_set.cursor, m);
            spice_marshall_msg_cursor_set(m, &cursor_set);
            break;
        }
    case QXL_CURSOR_HIDE:
        ccc->init_send_data(SPICE_MSG_CURSOR_HIDE);
        break;
    case QXL_CURSOR_TRAIL:
        {
            SpiceMsgCursorTrail cursor_trail;

            ccc->init_send_data(SPICE_MSG_CURSOR_TRAIL);
            cursor_trail.length = cmd->u.trail.length;
            cursor_trail.frequency = cmd->u.trail.frequency;
            spice_marshall_msg_cursor_trail(m, &cursor_trail);
        }
        break;
    default:
        spice_error("bad cursor command %d", cmd->type);
    }
}

static inline void red_marshall_inval(RedChannelClient *rcc,
                                      SpiceMarshaller *base_marshaller,
                                      RedCacheItem *cach_item)
{
    SpiceMsgDisplayInvalOne inval_one;

    rcc->init_send_data(SPICE_MSG_CURSOR_INVAL_ONE);
    inval_one.id = cach_item->id;

    spice_marshall_msg_cursor_inval_one(base_marshaller, &inval_one);
}

void CursorChannelClient::send_item(RedPipeItem *pipe_item)
{
    SpiceMarshaller *m = get_marshaller();
    CursorChannelClient *ccc = this;

    switch (pipe_item->type) {
    case RED_PIPE_ITEM_TYPE_CURSOR:
        red_marshall_cursor(ccc, m, SPICE_UPCAST(RedCursorPipeItem, pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_INVAL_ONE:
        red_marshall_inval(this, m, SPICE_CONTAINEROF(pipe_item, RedCacheItem, u.pipe_data));
        break;
    case RED_PIPE_ITEM_TYPE_CURSOR_INIT:
        reset_cursor_cache();
        red_marshall_cursor_init(this, m);
        break;
    case RED_PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
        reset_cursor_cache();
        init_send_data(SPICE_MSG_CURSOR_INVAL_ALL);
        break;
    default:
        spice_error("invalid pipe item type");
    }

    begin_send_message();
}

CursorChannel* cursor_channel_new(RedsState *server, int id,
                                  SpiceCoreInterfaceInternal *core,
                                  Dispatcher *dispatcher)
{
    spice_debug("create cursor channel");
    return new CursorChannel(server, id, core, dispatcher);
}

void cursor_channel_process_cmd(CursorChannel *cursor, RedCursorCmd *cursor_cmd)
{
    RedCursorPipeItem *cursor_pipe_item;
    bool cursor_show = false;

    spice_return_if_fail(cursor);
    spice_return_if_fail(cursor_cmd);

    cursor_pipe_item = cursor_pipe_item_new(cursor_cmd);

    switch (cursor_cmd->type) {
    case QXL_CURSOR_SET:
        cursor->cursor_visible = !!cursor_cmd->u.set.visible;
        cursor_channel_set_item(cursor, cursor_pipe_item);
        break;
    case QXL_CURSOR_MOVE:
        cursor_show = !cursor->cursor_visible;
        cursor->cursor_visible = true;
        cursor->cursor_position = cursor_cmd->u.position;
        break;
    case QXL_CURSOR_HIDE:
        cursor->cursor_visible = false;
        break;
    case QXL_CURSOR_TRAIL:
        cursor->cursor_trail_length = cursor_cmd->u.trail.length;
        cursor->cursor_trail_frequency = cursor_cmd->u.trail.frequency;
        break;
    default:
        spice_warning("invalid cursor command %u", cursor_cmd->type);
        red_pipe_item_unref(&cursor_pipe_item->base);
        return;
    }

    if (cursor->is_connected() &&
        (cursor->mouse_mode == SPICE_MOUSE_MODE_SERVER
         || cursor_cmd->type != QXL_CURSOR_MOVE
         || cursor_show)) {
        cursor->pipes_add(&cursor_pipe_item->base);
    } else {
        red_pipe_item_unref(&cursor_pipe_item->base);
    }
}

void cursor_channel_reset(CursorChannel *cursor)
{
    spice_return_if_fail(cursor);

    cursor_channel_set_item(cursor, NULL);
    cursor->cursor_visible = true;
    cursor->cursor_position.x = cursor->cursor_position.y = 0;
    cursor->cursor_trail_length = cursor->cursor_trail_frequency = 0;

    if (cursor->is_connected()) {
        cursor->pipes_add_type(RED_PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        if (!cursor->get_during_target_migrate()) {
            cursor->pipes_add_empty_msg(SPICE_MSG_CURSOR_RESET);
        }
        cursor->wait_all_sent(COMMON_CLIENT_TIMEOUT);
    }
}

static void cursor_channel_init_client(CursorChannel *cursor, CursorChannelClient *client)
{
    spice_return_if_fail(cursor);

    if (!cursor->is_connected()
        || cursor->get_during_target_migrate()) {
        spice_debug("during_target_migrate: skip init");
        return;
    }

    if (client)
        client->pipe_add_type(RED_PIPE_ITEM_TYPE_CURSOR_INIT);
    else
        cursor->pipes_add_type(RED_PIPE_ITEM_TYPE_CURSOR_INIT);
}

void cursor_channel_do_init(CursorChannel *cursor)
{
    cursor_channel_init_client(cursor, NULL);
}

void cursor_channel_set_mouse_mode(CursorChannel *cursor, uint32_t mode)
{
    spice_return_if_fail(cursor);

    cursor->mouse_mode = mode;
}

/**
 * Connect a new client to CursorChannel.
 */
void CursorChannel::on_connect(RedClient *client, RedStream *stream, int migration,
                               RedChannelCapabilities *caps)
{
    CursorChannelClient *ccc;

    spice_debug("add cursor channel client");
    ccc = cursor_channel_client_new(this, client, stream,
                                    migration,
                                    caps);
    if (ccc == NULL) {
        return;
    }

    ccc->ack_zero_messages_window();
    ccc->push_set_ack();

    cursor_channel_init_client(this, ccc);
}

CursorChannel::~CursorChannel()
{
    if (item) {
        red_pipe_item_unref(&item->base);
    }
}

CursorChannel::CursorChannel(RedsState *reds, uint32_t id,
                             SpiceCoreInterfaceInternal *core, Dispatcher *dispatcher):
    CommonGraphicsChannel(reds, SPICE_CHANNEL_CURSOR, id, RedChannel::HandleAcks, core, dispatcher)
{
    reds_register_channel(reds, this);
}
