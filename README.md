# mousegest

**Touchpad-style workspace swipes, on a mouse**

[![vibecoded](https://img.shields.io/badge/vibecoded-100%25-ff69b4)](#model-credits)
[![release](https://img.shields.io/github/v/release/Petyok/mousegest)](https://github.com/Petyok/mousegest/releases)
[![license](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

> ⚠️ This project is 100% vibe-coded slop — see [Model credits](#model-credits)
> for who to blame; not a single line was written by a human. It has — and will
> keep having — stupid bugs LLMs can't see. The kernel drops the grab when the
> process dies, so your mouse always comes back; use at your own risk.

Two mouse gestures for Hyprland:

- Hold **LMB+RMB** and drag horizontally → workspaces swipe 1:1, exactly like a
  3-finger touchpad swipe (`gesture = 3, horizontal, workspace`).
- Hold **mainMod (Super/Win) + LMB+RMB** and drag → the active window is carried
  across workspaces, one per `--move-step` mouse units, view following, for as
  long as you drag (Hyprland has no native "carry window" swipe, so this is
  quantised per notch rather than a smooth slide).

## How it works

It grabs the mouse's evdev stream and passes everything through unchanged. While
LMB+RMB are held it swallows the buttons/motion and instead drives a **synthetic
3-finger swipe** on a virtual `uinput` touchpad it creates. libinput recognises
that as a real gesture, so Hyprland does the actual pixel-proportional workspace
movement — no custom switching logic, no Hyprland plugin, survives updates.

If mainMod is held when the chord starts, it instead fires
`hyprctl dispatch movetoworkspace e±1` per notch (move-window mode). mainMod state
is read by observing (not grabbing) every keyboard exposing the `--mod-key`.

The event loop is fully event-driven: `epoll` over the mouse, keyboards and a
one-shot `timerfd` (the click-swallow window) — no timeouts, no polling.

Click swallowing: a button press is held back for a short window (`--window`,
default 35 ms). If the other button arrives in time → it's a gesture chord, both
presses are discarded. Otherwise the press is flushed as a normal click/hold.

## Install

One command (clones, builds, installs, sets up `/dev/uinput` and group access):

```sh
bash <(curl -fsSL https://raw.githubusercontent.com/Petyok/mousegest/main/install.sh)
```

Pass `--dry-run` to see every step without executing it.

Arch:

```sh
makepkg -si            # PKGBUILD is in the repo
```

By hand:

```sh
make
sudo make install      # -> /usr/local/bin/mousegest
sudo usermod -aG input "$USER"   # to read the mouse node; log out and back in
sudo modprobe uinput             # and see the udev rule in install.sh
```

## Usage

```sh
mousegest --sens 4
```

With no flags it grabs the first mouse it can. If that picks the wrong pointer,
narrow the scan by name or pin the node:

```sh
mousegest --match "Logitech" --sens 4
mousegest --grab /dev/input/event5 --sens 4
```

Device names come from `cat /proc/bus/input/devices`.

Autostart from `~/.config/hypr/hyprland.conf`:

```
exec-once = /usr/local/bin/mousegest --sens 4
```

## Options

- `--sens F`      mouse-unit → touchpad-unit gain (default 1.0). **4** feels good
                  on a 1000 Hz gaming mouse. Negative flips swipe direction.
- `--window MS`   chord-detect / click-swallow window (default 35).
- `--mod-key C`   evdev keycode that means mainMod (default 125 = KEY_LEFTMETA).
                  XKB remaps such as `ctrl:swap_lwin_lctl` do not change this:
                  evdev sees the raw keycode, so 125 stays correct.
- `--move-step F` mouse units per workspace in move-window mode (default 1500).
- `--move-min MS` minimum gap between workspace jumps, debounce (default 120).
- `--move-invert` flip move-window direction.
- `--match NAME`  restrict the scan to devices whose name contains NAME.
- `--grab /dev/input/eventN`  grab exactly this node, no scanning.

Nodes already grabbed by someone else are skipped, so with Interception Tools
running, `--match` lands on its virtual passthrough output rather than fighting
it for the physical node.

If stdin is not a terminal, mousegest runs as an Interception Tools plugin
instead (stdin → stdout):

```sh
intercept -g $DEVNODE | mousegest | uinput -d $DEVNODE
```

## Requirements

- Linux with `/dev/uinput`. No root at runtime if `/dev/uinput` is writable by
  your user (a udev ACL rule is enough); otherwise run as root or adjust
  permissions.
- `libevdev` and `pkg-config` to build.
- Hyprland configured with a 3-finger horizontal workspace gesture
  (`gesture = 3, horizontal, workspace`), which is the default.

Developed and used on Arch with Hyprland 0.55.2.

## Why a virtual touchpad

The obvious implementation is to watch the mouse and call `hyprctl dispatch
workspace` per notch. That gives you discrete jumps, not the 1:1 pixel-following
slide a real touchpad gesture produces. Emitting a synthetic multitouch swipe
instead hands the whole animation to libinput and Hyprland, so the motion is
identical to the real thing and nothing has to be reimplemented.

Getting libinput to accept the virtual device took two non-obvious constraints,
both found by trial:

- **Do not advertise `ABS_PRESSURE` or `ABS_MT_TOUCH_MAJOR`.** If they are
  present, libinput compares them against its touch-down thresholds; the values a
  synthetic device reports fall below those thresholds, so every contact is
  classified as hover. The device shows up correctly and is then silently ignored.
- **Do not use Apple's vendor id (`0x05ac`).** libinput enables model quirks for
  Apple touchpads and starts expecting the real bcm5974 protocol, which this
  device does not speak.

`--match` iterates the event nodes and takes the first one it can actually grab,
which makes it robust against event-node renumbering across reboots and against
Interception Tools holding the physical node (mousegest then lands on its virtual
passthrough output).

## Model credits

Built end-to-end by Claude models in sessions with a human — the design, the C,
the libinput reverse-engineering above, this README and the packaging. Not a
single line was typed by a person.

The human contributed the idea, the hardware, the testing, and the swearing.

## License

AGPL-3.0-only. See [LICENSE](LICENSE).
