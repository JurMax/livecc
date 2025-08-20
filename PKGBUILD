pkgname=livecc
pkgver=0.2
pkgrel=1
pkgdesc="Automatic live reload for C/C++."
arch=('x86_64')
url="https://github.com/JurMax/livecc/"
provides=('livecc')
sha256sums=()

build() {
    cd ../
    make
}


package() {
    install -Dm755 ../livecc "$pkgdir/usr/bin/livecc"
}
