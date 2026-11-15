"""Run the host's actual confirmation helper against a tiny simulated window.

Usage: python tests/title_confirm.py (with the C compiler on PATH).
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
host = (root / "host.c").read_text()
start = host.index("static int title_confirm_yes(void) {")
helper = host[start:host.index("\n}", start) + 2]
harness = r"""
#include <assert.h>
#include <stdint.h>
#define PSP_CTRL_CROSS 0x4000u
static int state, choice, duel, opening, acknowledge, stop_held;
static int elapsed, pressed, released, held;
static int psp_duel_manager_start_requested(void) { return duel; }
static int psp_title_confirm_state(void) { return state; }
static int stable_title_confirm_choice(void) { return choice; }
static void psp_title_menu_request(uint32_t mask) {
    if (mask) { assert(mask == PSP_CTRL_CROSS); pressed++; }
    else released++;
    held = mask != 0;
}
static void psp_ctrl_set(uint32_t mask, int x, int y) {
    assert(x == 128 && y == 128 && (mask == 0 || mask == PSP_CTRL_CROSS));
}
static void Sleep(int ms) {
    elapsed += ms;
    if (opening && elapsed >= 30) state = 3;
    if (held && acknowledge && elapsed >= 100) state = 4;
    if (held && stop_held) duel = 1;
}
""" + helper + r"""
static void reset(void) {
    state = 3; choice = 1; duel = opening = stop_held = 0;
    acknowledge = 1; elapsed = pressed = released = held = 0;
}
int main(void) {
    reset(); assert(title_confirm_yes());
    assert(pressed == 1 && released == 1 && !held);
    reset(); state = 2; opening = 1; assert(title_confirm_yes());
    assert(pressed == 1 && released == 1 && !held);
    reset(); choice = 0; assert(!title_confirm_yes() && !pressed);
    reset(); duel = 1; assert(!title_confirm_yes() && !pressed);
    reset(); state = 2; assert(!title_confirm_yes() && !pressed);
    reset(); acknowledge = 0; assert(!title_confirm_yes());
    assert(pressed == 1 && released == 1 && !held);
    reset(); acknowledge = 0; stop_held = 1; assert(!title_confirm_yes());
    assert(pressed == 1 && released == 1 && !held);
}
"""
with tempfile.TemporaryDirectory(prefix="seto-confirm-") as directory:
    source = Path(directory) / "confirm.c"
    exe = Path(directory) / "confirm.exe"
    source.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("title confirmation: 7 cases passed")
