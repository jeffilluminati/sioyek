#!/usr/bin/env bash
# Builds bench/sioyek_bench against the object files of the qmake build in the repository root.
# Run qmake (e.g. through build_mac.sh) once before using this script.
set -e
cd "$(dirname "$0")/.."

MAKE_PARALLEL=${MAKE_PARALLEL:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}

# print a variable of the qmake generated Makefile
mkvar() {
    make -s -f Makefile -f - "print-$1" <<'EOF'
print-%:
	@echo $($*)
EOF
}

OBJECTS=$(mkvar OBJECTS)
CXX=$(mkvar CXX)
CXXFLAGS=$(mkvar CXXFLAGS)
INCPATH=$(mkvar INCPATH)
LINK=$(mkvar LINK)
LFLAGS=$(mkvar LFLAGS)
LIBS=$(mkvar LIBS)

# only the application's object files are needed, not the app bundle
make -j"$MAKE_PARALLEL" $OBJECTS

# main.o is linked as is (it defines globals the rest of the code needs), the benchmark and
# the test just use a different entry point.
build() {
    local name=$1 entry=$2
    $CXX -c $CXXFLAGS $INCPATH -o "bench/$name.o" "bench/$name.cpp"
    $LINK $LFLAGS -Wl,-e,"$entry" -o "bench/$name" "bench/$name.o" $OBJECTS $LIBS
    echo "built bench/$name"
}

build sioyek_bench _bench_main
build equivalence_test _test_main
