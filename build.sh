#!/usr/bin/env bash
set -eu

info() {
	echo -e "\033[33m$1\033[m"
}

die() {
	echo -e "\033[31m$1\033[m"
	exit 1
}

build_type="Release"

while test $# -ge 1; do
	case "$1" in
		"clean") rm -rf out markov ;;
		"debug") build_type="Debug" ;;
		*) die "Unknown argument '$1'" ;;
	esac

	shift
done

mkdir -p out
cd out
cmake -DCMAKE_BUILD_TYPE="$build_type" -GNinja ..
ninja -j $(nproc)

