#!/bin/bash
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Copyright (C) 2026 Marcin Bukat
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

# Runs a2dp_test_sink.py with WirePlumber stopped, so the source doesn't
# pick PipeWire's own A2DP sink, and starts WirePlumber again however
# the test sink ends. Arguments go to a2dp_test_sink.py.

sink="$(dirname "$(readlink -f "$0")")/a2dp_test_sink.py"
stopped=

restore() {
    if [ -n "$stopped" ]; then
        echo "starting WirePlumber again"
        systemctl --user start wireplumber
        stopped=
    fi
}
trap restore EXIT

# Ctrl-C goes to the test sink, which cleans up and quits; the wrapper
# only has to outlive it. A handler rather than ignoring the signal, as
# an ignored signal would stay ignored in the test sink too.
trap ':' INT TERM HUP

if systemctl --user --quiet is-active wireplumber; then
    echo "stopping WirePlumber, desktop audio is off until the test sink quits"
    systemctl --user stop wireplumber || exit 1
    stopped=1
fi

python3 "$sink" "$@"
