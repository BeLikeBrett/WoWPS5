#!/usr/bin/env bash
# Pinned, static TLS dependencies for the console's Wine Unix libraries.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk="$root/app/.deps/native/ps5-payload-sdk"
src="$root/work/tls-src" build="$root/work/tls-build" prefix="$root/build/ps5-tls"
mkdir -p "$src" "$build" "$prefix"
fetch() {
    local file=$1 hash=$2 url=$3
    [[ -f "$src/$file" ]] || curl -fL --retry 2 --max-time 120 "$url" -o "$src/$file"
    printf '%s  %s\n' "$hash" "$src/$file" | sha256sum -c -
    [[ -d "$src/${file%.tar.*}" ]] || tar -xf "$src/$file" -C "$src"
}
fetch gmp-6.3.0.tar.xz a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898 https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
fetch nettle-3.10.2.tar.gz fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5 https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz
fetch gnutls-3.8.13.tar.xz ffed8ec1bf09c2426d4f14aae377de4753b53e537d685e604e99a8b16ca9c97e https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.13.tar.xz
# The title has no /dev/urandom. Use the console's secure random service.
for backend in sysrng-linux.c sysrng-getentropy.c; do
    cmp -s "$root/runtime/wine-ps5/tls-entropy.c" "$src/gnutls-3.8.13/lib/nettle/$backend" || \
        cp "$root/runtime/wine-ps5/tls-entropy.c" "$src/gnutls-3.8.13/lib/nettle/$backend"
done
export CC="$sdk/bin/prospero-clang" AR="$sdk/bin/llvm-ar" RANLIB="$sdk/bin/llvm-ranlib" NM="$sdk/bin/llvm-nm"
export CFLAGS="-O2 -fPIC" CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
jobs=${JOBS:-8}
compile() {
    local package=$1; shift
    mkdir -p "$build/$package"
    (
        cd "$build/$package"
        if [[ ! -f Makefile ]]; then
            "$src/$package/configure" --host=x86_64-unknown-freebsd --prefix="$prefix" --disable-shared --enable-static "$@" > configure.log 2>&1 || exit
        fi
        if [[ $package == nettle-* ]]; then
            make -j"$jobs" all-here > build.log 2>&1 && make install-here > install.log 2>&1
        elif [[ $package == gnutls-* ]]; then
            make -C lib -j"$jobs" > build.log 2>&1 && make -C lib install > install.log 2>&1
        else
            make -j"$jobs" > build.log 2>&1 && make install > install.log 2>&1
        fi
    ) || { tail -n 60 "$build/$package/"*.log; exit 1; }
    echo "built $package"
}
compile gmp-6.3.0 --disable-assembly --disable-cxx
compile nettle-3.10.2 --disable-assembler --disable-documentation
compile gnutls-3.8.13 --with-included-libtasn1 --with-included-unistring --without-idn --without-p11-kit \
    --without-tpm --without-tpm2 --without-zlib --without-brotli --without-zstd \
    --disable-hardware-acceleration --disable-tools --disable-tests --disable-doc --disable-cxx --disable-nls --disable-libdane
printf '%s\n' "Static PS5 TLS: GMP 6.3.0, Nettle 3.10.2, GnuTLS 3.8.13" > "$prefix/PROVENANCE.txt"
mkdir -p "$prefix/licenses"
for package in gmp-6.3.0 nettle-3.10.2 gnutls-3.8.13; do
    mkdir -p "$prefix/licenses/$package"
    cp "$src/$package/"COPYING* "$prefix/licenses/$package/"
done
cp "$root/runtime/wine-ps5/tls-entropy.c" "$prefix/licenses/console-entropy-source.c"
