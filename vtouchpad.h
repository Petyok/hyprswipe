#ifndef VTOUCHPAD_H
#define VTOUCHPAD_H

/*
 * Virtual multitouch touchpad that clones the bcm5974 ABS geometry so that
 * libinput's gesture detector treats synthetic contacts exactly like the real
 * Apple trackpad. We drive a 3-finger horizontal swipe, which Hyprland maps via
 * `gesture = 3, horizontal, workspace` -- i.e. the same code path the touchpad
 * already uses, so motion is 1:1.
 */

struct vtouchpad {
    struct libevdev_uinput *uidev;
    double acc_x;   /* accumulated horizontal travel, in touchpad ABS units */
    int active;     /* swipe in progress */
};

/* Create the virtual touchpad. Returns 0 on success, -errno on failure. */
int vtouchpad_create(struct vtouchpad *tp);

/* Put 3 fingers down at the rest position and begin a swipe gesture. */
void vtouchpad_begin(struct vtouchpad *tp);

/* Move the 3 contacts horizontally by `dx` touchpad units (signed). */
void vtouchpad_move(struct vtouchpad *tp, double dx);

/* Lift the fingers; libinput emits SWIPE_END and Hyprland commits the swipe. */
void vtouchpad_end(struct vtouchpad *tp);

void vtouchpad_destroy(struct vtouchpad *tp);

#endif
