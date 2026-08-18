#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "vtouchpad.h"

/* Geometry cloned from the bcm5974 (event8) on this machine. */
#define ABS_X_MIN   (-4620)
#define ABS_X_MAX   (5140)
#define ABS_Y_MIN   (-150)
#define ABS_Y_MAX   (6600)
#define RES_X       93   /* units/mm, ~105mm wide trackpad */
#define RES_Y       92   /* units/mm, ~73mm tall trackpad  */

#define CENTER_X    (((ABS_X_MIN) + (ABS_X_MAX)) / 2)   /* 260  */
#define CENTER_Y    (((ABS_Y_MIN) + (ABS_Y_MAX)) / 2)   /* 3225 */
#define FINGER_SPREAD 900    /* horizontal gap between the 3 contacts (~10mm) */
#define TOUCH_MAJOR  256
#define PRESSURE     64
#define MARGIN       64      /* keep contacts off the very edge */

static const int NFINGERS = 3;

static int finger_base_x(int i)
{
    /* fingers placed side by side along the swipe axis */
    return CENTER_X + (i - 1) * FINGER_SPREAD;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void enable_abs(struct libevdev *dev, int code, int min, int max, int res)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.minimum = min;
    ai.maximum = max;
    ai.resolution = res;
    libevdev_enable_event_code(dev, EV_ABS, code, &ai);
}

int vtouchpad_create(struct vtouchpad *tp)
{
    int rc;
    struct libevdev *dev = libevdev_new();
    if (!dev) return -ENOMEM;

    libevdev_set_name(dev, "mouse_gest virtual touchpad");
    libevdev_set_id_bustype(dev, BUS_USB);
    libevdev_set_id_vendor(dev, 0x1d6b);   /* neutral, avoid Apple quirks */
    libevdev_set_id_product(dev, 0x0abc);
    libevdev_set_id_version(dev, 0x0001);

    libevdev_enable_property(dev, INPUT_PROP_POINTER);
    libevdev_enable_property(dev, INPUT_PROP_BUTTONPAD);

    libevdev_enable_event_type(dev, EV_SYN);

    libevdev_enable_event_type(dev, EV_KEY);
    libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOUCH, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOOL_FINGER, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOOL_DOUBLETAP, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOOL_TRIPLETAP, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOOL_QUADTAP, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOOL_QUINTTAP, NULL);

    libevdev_enable_event_type(dev, EV_ABS);
    enable_abs(dev, ABS_X, ABS_X_MIN, ABS_X_MAX, RES_X);
    enable_abs(dev, ABS_Y, ABS_Y_MIN, ABS_Y_MAX, RES_Y);
    enable_abs(dev, ABS_MT_SLOT, 0, 15, 0);
    enable_abs(dev, ABS_MT_POSITION_X, ABS_X_MIN, ABS_X_MAX, RES_X);
    enable_abs(dev, ABS_MT_POSITION_Y, ABS_Y_MIN, ABS_Y_MAX, RES_Y);
    enable_abs(dev, ABS_MT_TRACKING_ID, 0, 65535, 0);

    rc = libevdev_uinput_create_from_device(dev,
            LIBEVDEV_UINPUT_OPEN_MANAGED, &tp->uidev);
    libevdev_free(dev);
    if (rc != 0) {
        tp->uidev = NULL;
        return rc;
    }
    tp->acc_x = 0;
    tp->active = 0;
    return 0;
}

static void w(struct vtouchpad *tp, unsigned int type, unsigned int code, int val)
{
    libevdev_uinput_write_event(tp->uidev, type, code, val);
}

static void syn(struct vtouchpad *tp)
{
    libevdev_uinput_write_event(tp->uidev, EV_SYN, SYN_REPORT, 0);
}

void vtouchpad_begin(struct vtouchpad *tp)
{
    if (tp->active) return;
    tp->acc_x = 0;

    for (int i = 0; i < NFINGERS; i++) {
        w(tp, EV_ABS, ABS_MT_SLOT, i);
        w(tp, EV_ABS, ABS_MT_TRACKING_ID, 100 + i);
        w(tp, EV_ABS, ABS_MT_POSITION_X, finger_base_x(i));
        w(tp, EV_ABS, ABS_MT_POSITION_Y, CENTER_Y);
    }
    w(tp, EV_KEY, BTN_TOUCH, 1);
    w(tp, EV_KEY, BTN_TOOL_TRIPLETAP, 1);
    /* legacy single-touch mirror */
    w(tp, EV_ABS, ABS_X, finger_base_x(1));
    w(tp, EV_ABS, ABS_Y, CENTER_Y);
    syn(tp);
    tp->active = 1;
}

void vtouchpad_move(struct vtouchpad *tp, double dx)
{
    if (!tp->active) return;
    tp->acc_x += dx;
    int off = (int)(tp->acc_x);
    for (int i = 0; i < NFINGERS; i++) {
        int x = clampi(finger_base_x(i) + off,
                       ABS_X_MIN + MARGIN, ABS_X_MAX - MARGIN);
        w(tp, EV_ABS, ABS_MT_SLOT, i);
        w(tp, EV_ABS, ABS_MT_POSITION_X, x);
    }
    int xc = clampi(finger_base_x(1) + off,
                    ABS_X_MIN + MARGIN, ABS_X_MAX - MARGIN);
    w(tp, EV_ABS, ABS_X, xc);
    syn(tp);
}

void vtouchpad_end(struct vtouchpad *tp)
{
    if (!tp->active) return;
    for (int i = 0; i < NFINGERS; i++) {
        w(tp, EV_ABS, ABS_MT_SLOT, i);
        w(tp, EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    w(tp, EV_KEY, BTN_TOUCH, 0);
    w(tp, EV_KEY, BTN_TOOL_TRIPLETAP, 0);
    syn(tp);
    tp->active = 0;
    tp->acc_x = 0;
}

void vtouchpad_destroy(struct vtouchpad *tp)
{
    if (tp->uidev) {
        libevdev_uinput_destroy(tp->uidev);
        tp->uidev = NULL;
    }
}
