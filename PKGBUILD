pkgname=livecc
pkgver=0.4.0
pkgrel=1
pkgdesc="Automatic live reload for C/C++."
arch=('x86_64')
url="https://github.com/JurMax/livecc/"
provides=('livecc')
depends=('mold' 'clang' 'libc++')
sha256sums=()

pkgver() {
    cd ".."
    sed -n 's/.*version\s*=\s*\"\(.*\)\"\s*;/\1/p' src/version.hpp
}

build() {
    cd ../
    make
}

package() {
    install -Dm755 ../livecc "$pkgdir/usr/bin/livecc"
}
