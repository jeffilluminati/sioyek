#!/usr/bin/env bash
# Optimized macOS build: Homebrew LLVM clang + lld, -O3 and full LTO across sioyek and mupdf.
# prerequisites: brew install llvm lld jpeg-turbo, and Qt 6 (qmake and macdeployqt on PATH, e.g. via .env)
# usage: ./build_mac_llvm_lto.sh [clean]
set -euo pipefail
cd "$(dirname "$0")"

# Homebrew include/library paths exported by a login shell would pull Homebrew's libc++ and other
# libraries into the build, keep them out.
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
if [ -f .env ]; then source .env; fi

LLVM=$(brew --prefix llvm)/bin
LLD=$(brew --prefix lld)/bin/ld64.lld
SDK=$(xcrun --show-sdk-path)
JOBS=${MAKE_PARALLEL:-$(sysctl -n hw.ncpu)}
export MACOSX_DEPLOYMENT_TARGET=$(grep -oE "QMAKE_MACOSX_DEPLOYMENT_TARGET = [0-9]+" pdf_viewer_build_config.pro | grep -oE "[0-9]+$").0

OPT="-O3 -flto"
# Qt and the system use the SDK's libc++, so compile against its headers rather than Homebrew's.
SDK_CXX="-nostdinc++ -isystem $SDK/usr/include/c++/v1"
LTO_LDFLAGS="-flto --ld-path=$LLD -Wl,--lto-O3"

if [[ ${1:-} == clean ]]; then
	make -C mupdf clean
	if [ -f Makefile ]; then make distclean; fi
fi

./mupdf-patches/apply.sh

# See build_mac.sh for libjpeg-turbo and macOS's zlib.
MUPDF_JPEG=""
for prefix in /opt/homebrew/opt/jpeg-turbo /usr/local/opt/jpeg-turbo; do
	if [ -z "$MUPDF_JPEG" ] && [ -f "$prefix/lib/libjpeg.a" ]; then
		MUPDF_JPEG="USE_SYSTEM_LIBJPEG=yes SYS_LIBJPEG_CFLAGS=-I$prefix/include SYS_LIBJPEG_LIBS=$prefix/lib/libjpeg.a"
	fi
done
# llvm-ar/llvm-ranlib index the LTO bitcode, Apple's ar can't read bitcode from a newer LLVM.
# (mupdf's makefile doesn't notice changed flags, use `clean` after changing them)
make -C mupdf -j"$JOBS" build=release HAVE_GLUT=no USE_SYSTEM_ZLIB=yes \
	CC="$LLVM/clang" CXX="$LLVM/clang++" AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" \
	XCFLAGS="$OPT -isysroot $SDK" XCXXFLAGS="$SDK_CXX" XLDFLAGS="$LTO_LDFLAGS" \
	$MUPDF_JPEG libs build/release/libmupdf-threads.a

qmake "CONFIG+=non_portable" \
	"QMAKE_CC=$LLVM/clang" "QMAKE_CXX=$LLVM/clang++" \
	"QMAKE_LINK=$LLVM/clang++" "QMAKE_LINK_C=$LLVM/clang" \
	"QMAKE_CFLAGS_RELEASE=$OPT -DNDEBUG" \
	"QMAKE_CXXFLAGS_RELEASE=$OPT -DNDEBUG $SDK_CXX" \
	"QMAKE_OBJECTIVE_CFLAGS_RELEASE=$OPT -DNDEBUG $SDK_CXX" \
	"QMAKE_LFLAGS_RELEASE=$LTO_LDFLAGS" \
	pdf_viewer_build_config.pro
make -j"$JOBS"

rm -rf build
mkdir build
mv sioyek.app build/
cp -r pdf_viewer/shaders build/sioyek.app/Contents/Resources/shaders
cp pdf_viewer/prefs.config pdf_viewer/prefs_user.config pdf_viewer/keys.config pdf_viewer/keys_user.config tutorial.pdf build/sioyek.app/Contents/Resources/

INFO_PLIST="build/sioyek.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :LSEnvironment dict" "$INFO_PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :LSEnvironment:PATH string $PATH" "$INFO_PLIST" 2>/dev/null || /usr/libexec/PlistBuddy -c "Set :LSEnvironment:PATH $PATH" "$INFO_PLIST"

macdeployqt build/sioyek.app
codesign --force --deep --sign - build/sioyek.app
