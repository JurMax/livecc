pkgname=live_cc
pkgver=0.1
pkgrel=1
pkgdesc="Automatic live reload for C++."
arch=('x86_64')
url=""
provides=('live_cc')
sha256sums=()

build() {
    cd ../
    make
}


package() {
    install -Dm755 ../live_cc "$pkgdir/usr/bin/live_cc"
}
