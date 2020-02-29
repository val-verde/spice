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

#ifndef INPUTS_CHANNEL_H_
#define INPUTS_CHANNEL_H_

// Inputs channel, dealing with keyboard, mouse, tablet.
// This include should only be used by reds.c and inputs-channel.c

#include <stdint.h>
#include <spice/vd_agent.h>

#include "red-channel.h"

#include "push-visibility.h"

struct InputsChannel final: public RedChannel
{
    InputsChannel(RedsState *reds);
    ~InputsChannel();
    void on_connect(RedClient *client, RedStream *stream, int migration,
                    RedChannelCapabilities *caps) override;

    VDAgentMouseState mouse_state;
    bool src_during_migrate;
    SpiceTimer *key_modifiers_timer;

    // actual ideal modifier states, that the guest should have
    uint8_t modifiers;
    // current pressed modifiers
    uint8_t modifiers_pressed;

    SpiceKbdInstance *keyboard;
    SpiceMouseInstance *mouse;
    SpiceTabletInstance *tablet;

public:
    const VDAgentMouseState *get_mouse_state();
    void set_tablet_logical_size(int x_res, int y_res);

    int set_keyboard(SpiceKbdInstance *keyboard);
    int set_mouse(SpiceMouseInstance *mouse);
    int set_tablet(SpiceTabletInstance *tablet);
    bool has_tablet() const;
    void detach_tablet(SpiceTabletInstance *tablet);

    bool is_src_during_migrate() const;
    void release_keys();
};

InputsChannel* inputs_channel_new(RedsState *reds);

RedsState* spice_tablet_state_get_server(SpiceTabletState *dev);

#include "pop-visibility.h"

#endif /* INPUTS_CHANNEL_H_ */
