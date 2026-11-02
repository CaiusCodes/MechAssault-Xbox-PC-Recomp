#!/bin/sh
# host unit tests for platform-neutral runtime code (Linux/macOS, any C++20 compiler)
set -e
cd "$(dirname "$0")/.."
c++ -std=c++20 -O1 -Isdk/runtime tests/frame_stats_test.cpp sdk/runtime/core/frame_stats.cpp -o /tmp/frame_stats_test -lpthread
/tmp/frame_stats_test
# game import: needs a folder with the game's default.xbe (GAME_DIR, default /home/claude/game); builds small
# images from a subset with make_xiso.py
GAME_DIR=${GAME_DIR:-/home/claude/game}
if [ -f "$GAME_DIR/default.xbe" ]; then
    T=$(mktemp -d)
    mkdir -p "$T/sub/stream" "$T/sub/romdata/common" "$T/sub/romdata/area" "$T/work"
    cp "$GAME_DIR/default.xbe" "$T/sub/"
    cp "$GAME_DIR"/stream/*.sth "$T/sub/stream/"
    cp "$GAME_DIR/romdata/common/usa.pak" "$GAME_DIR/romdata/common/nus_config_x.txt" "$T/sub/romdata/common/"
    cp -r "$GAME_DIR/romdata/area/opening" "$T/sub/romdata/area/"
    python3 tests/make_xiso.py "$T/sub" "$T/work/sub.iso"
    python3 tests/make_xiso.py --full "$T/sub" "$T/sub_full.iso"
    c++ -std=c++20 -O1 -Isdk/runtime tests/game_import_test.cpp sdk/runtime/core/game_import.cpp \
        sdk/runtime/kernel/disc.cpp sdk/runtime/kernel/crypto.cpp -o "$T/game_import_test"
    "$T/game_import_test" "$T/sub" "$T/work/sub.iso" "$T/sub_full.iso" "$T/work"
    rm -rf "$T"
fi
