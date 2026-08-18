# Maintainer: Petr Netupskii <petruha@users.noreply.github.com>
pkgname=mousegest
pkgver=0.1.0
pkgrel=1
pkgdesc="LMB+RMB mouse gestures for Hyprland via a synthetic 3-finger touchpad swipe"
arch=(x86_64 aarch64)
url="https://github.com/Petyok/mousegest"
license=('AGPL-3.0-only')
depends=(libevdev)
makedepends=(pkgconf)
optdepends=(
  'hyprland: the compositor whose workspace gesture this drives'
  'interception-tools: run as a udevmon plugin instead of grabbing the node directly'
)
install=mousegest.install
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
# Replaced by `updpkgsums` at release time.
sha256sums=('SKIP')

build() {
  make -C "$pkgname-$pkgver"
}

package() {
  cd "$pkgname-$pkgver"
  install -Dm755 mousegest "$pkgdir/usr/bin/mousegest"
  install -Dm644 LICENSE  "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
  # The virtual touchpad goes through uinput, so ship the module load and the
  # uaccess tag that makes /dev/uinput writable by the seat user.
  install -Dm644 /dev/stdin "$pkgdir/usr/lib/modules-load.d/$pkgname-uinput.conf" <<<'uinput'
  install -Dm644 /dev/stdin "$pkgdir/usr/lib/udev/rules.d/99-$pkgname-uinput.rules" \
    <<<'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"'
}
