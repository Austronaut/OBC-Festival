#!/usr/bin/env bash
# Open the CC1101 receiver flowgraph in GNU Radio Companion.
#
# PYTHONNOUSERSITE=1 makes Python ignore pip packages in ~/.local.
# A pip-installed numpy 2.x there breaks Ubuntu's apt GNU Radio, which
# is compiled against the system numpy 1.x ("ImportError: initialization
# failed"). Flowgraphs started with GRC's Run button inherit this setting.
cd "$(dirname "$0")" || exit 1
PYTHONNOUSERSITE=1 exec gnuradio-companion cc1101_rx.grc "$@"
