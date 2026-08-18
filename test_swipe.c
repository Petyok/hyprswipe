/*
 * Standalone test: create the virtual touchpad and perform a slow 3-finger
 * horizontal swipe so we can watch whether Hyprland reacts (workspace follows).
 *
 * Usage: ./test_swipe [total_units] [steps] [step_delay_ms]
 *   total_units    signed; positive = swipe right (default +1200)
 *   steps          number of move increments (default 60)
 *   step_delay_ms  delay between increments (default 16ms ~ 60Hz)
 */
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "vtouchpad.h"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    double total = (argc > 1) ? atof(argv[1]) : 1200.0;
    int steps    = (argc > 2) ? atoi(argv[2]) : 60;
    long delay   = (argc > 3) ? atol(argv[3]) : 16;
    if (steps < 1) steps = 1;

    struct vtouchpad tp;
    int rc = vtouchpad_create(&tp);
    if (rc != 0) {
        fprintf(stderr, "vtouchpad_create failed: %d (%s)\n", rc,
                rc < 0 ? "check /dev/uinput access" : "");
        return 1;
    }
    fprintf(stderr, "virtual touchpad created; "
            "waiting 1s for libinput/Hyprland to enumerate it...\n");
    sleep_ms(1000);

    fprintf(stderr, "begin swipe (total=%.0f units, %d steps)\n", total, steps);
    vtouchpad_begin(&tp);
    sleep_ms(delay);

    double per = total / steps;
    for (int i = 0; i < steps; i++) {
        vtouchpad_move(&tp, per);
        sleep_ms(delay);
    }

    fprintf(stderr, "end swipe\n");
    vtouchpad_end(&tp);
    sleep_ms(200);

    vtouchpad_destroy(&tp);
    return 0;
}
