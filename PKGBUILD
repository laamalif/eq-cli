# Maintainer: local

pkgname=eq-cli
pkgver=0.3.2.2
pkgrel=1
pkgdesc='Headless PipeWire DSP runner for EasyEffects-compatible presets (EQ, convolver, limiter)'
arch=(x86_64 aarch64)
url='https://github.com/laamalif/eq-cli/'
license=('MIT')
depends=(
  'pipewire'
  'lilv'
  'nlohmann-json'
  'libsndfile'
  'zita-convolver'
)
makedepends=(
  'cmake'
  'ninja'
  'pkgconf'
)
optdepends=(
  'lsp-plugins: LSP LV2 equalizer plugin required at runtime'
)
provides=(eq-cli)
source=()
sha256sums=()

build() {
    cd "${startdir}"

    cmake \
      -B build \
      -S "${startdir}" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr

    cmake --build build
  }

  package() {
    cd "${startdir}"

    install -Dm755 build/eq-cli "${pkgdir}/usr/bin/eq-cli"
    install -Dm644 README.md "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}
