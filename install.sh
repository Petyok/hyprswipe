#!/usr/bin/env bash
# mousegest installer: deps, build, /dev/uinput + /dev/input access, Hyprland hint.
# Idempotent. Usage: ./install.sh [--dry-run]
#
# One-liner (clones itself into a temp dir first):
#   bash <(curl -fsSL https://raw.githubusercontent.com/Petyok/mousegest/main/install.sh)
set -euo pipefail

REPO_URL=https://github.com/Petyok/mousegest.git
USER_NAME=${USER:-$(id -un)}
UDEV_RULE=/etc/udev/rules.d/99-mousegest-uinput.rules
MODULE_CONF=/etc/modules-load.d/mousegest-uinput.conf

DRY_RUN=0
case "${1:-}" in
  --dry-run) DRY_RUN=1 ;;
  "") ;;
  *) echo "usage: $0 [--dry-run]" >&2; exit 2 ;;
esac

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
# Every side effect goes through run(): printed always, executed only for real.
run() {
  printf '+ %s\n' "$*"
  [ "$DRY_RUN" -eq 1 ] || "$@"
}

step "1. Locate sources"
# Piped from curl there is no repo around us, so fetch one.
SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || echo /nonexistent)
if [ -f "$SELF_DIR/mousegest.c" ]; then
  REPO=$SELF_DIR
  say "building from $REPO"
else
  REPO=$(mktemp -d)
  say "no sources next to this script, cloning into $REPO"
  run git clone --depth 1 "$REPO_URL" "$REPO"
  [ "$DRY_RUN" -eq 1 ] && REPO=$SELF_DIR
fi

step "2. Dependencies"
distro=unknown
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  distro=$(. /etc/os-release && printf '%s' "${ID:-unknown}")
fi
say "distro ID: $distro"
case "$distro" in
  arch|archarm|manjaro|endeavouros)
    # pacman -T prints only what no installed package provides.
    missing=$(pacman -T libevdev pkgconf gcc make || true)
    if [ -n "$missing" ]; then run sudo pacman -S --needed -- $missing
    else say "libevdev, pkgconf, gcc, make already present"; fi ;;
  debian|ubuntu|linuxmint|pop)
    run sudo apt-get update
    run sudo apt-get install -y libevdev-dev pkg-config build-essential ;;
  fedora|rhel|centos)
    run sudo dnf install -y libevdev-devel pkgconf-pkg-config gcc make ;;
  *)
    say "unknown distro: install libevdev headers, pkg-config, a C compiler and make yourself" ;;
esac

step "3. Build"
run make -C "$REPO"

step "4. Install binary"
run sudo make -C "$REPO" install       # -> /usr/local/bin/mousegest

step "5. /dev/uinput access"
# The virtual touchpad is created through uinput, so the module must be loaded
# and the node writable without root. TAG+="uaccess" hands it to the seat user.
if [ ! -e "$MODULE_CONF" ]; then
  run sudo install -Dm644 /dev/stdin "$MODULE_CONF" <<<'uinput'
else
  say "$MODULE_CONF exists"
fi
run sudo modprobe uinput
if [ ! -e "$UDEV_RULE" ]; then
  run sudo install -Dm644 /dev/stdin "$UDEV_RULE" \
    <<<'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"'
else
  say "$UDEV_RULE exists"
fi
run sudo udevadm control --reload-rules
run sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput

step "6. /dev/input access"
# Reading the mouse's evdev node needs the input group on most distros.
if id -nG "$USER_NAME" | tr ' ' '\n' | grep -qx input; then
  say "$USER_NAME is already in group input"
else
  run sudo usermod -aG input "$USER_NAME"
  say "added $USER_NAME to group input -- LOG OUT AND BACK IN for this to apply"
fi

step "Done"
cat <<'HINT'
Try it now (Ctrl-C to stop):

    mousegest --sens 4

Then autostart it from ~/.config/hypr/hyprland.conf:

    exec-once = /usr/local/bin/mousegest --sens 4

With no flags mousegest picks the first mouse it can grab. If you have several
pointers and it takes the wrong one, narrow the scan:

    mousegest --match "Logitech" --sens 4    # substring of the device name
    mousegest --grab /dev/input/event5       # or pin the node outright

Device names come from:  cat /proc/bus/input/devices
HINT
