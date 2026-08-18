# Maintainer: Petr Netupskii <petruha@users.noreply.github.com>
pkgname=hyprswipe
pkgver=0.1.0
pkgrel=1
pkgdesc="LMB+RMB mouse gestures for Hyprland via a synthetic 3-finger touchpad swipe"
arch=(x86_64 aarch64)
url="https://github.com/Petyok/hyprswipe"
license=('AGPL-3.0-only')
depends=(libevdev)
makedepends=(pkgconf)
optdepends=(
  'hyprland: the compositor whose workspace gesture this drives'
  'interception-tools: run as a udevmon plugin instead of grabbing the node directly'
)
install=hyprswipe.install
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
# sha256 of the v0.1.0 release tarball; refresh with `updpkgsums` on release.
sha256sums=('42f614123fa6915798d066e5936ba260030e2e0b7e1194742a3edb8122f223b9')

build() {
  make -C "$pkgname-$pkgver"
}

package() {
  cd "$pkgname-$pkgver"
  install -Dm755 hyprswipe "$pkgdir/usr/bin/hyprswipe"
  install -Dm644 LICENSE  "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
  # The virtual touchpad goes through uinput, so ship the module load and the
  # uaccess tag that makes /dev/uinput writable by the seat user.
  install -Dm644 /dev/stdin "$pkgdir/usr/lib/modules-load.d/$pkgname-uinput.conf" <<<'uinput'
  install -Dm644 /dev/stdin "$pkgdir/usr/lib/udev/rules.d/99-$pkgname-uinput.rules" \
    <<<'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"'
}
