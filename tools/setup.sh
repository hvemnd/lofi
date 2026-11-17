#!/bin/sh
# Fetch fm1-base (and its felucca-bsp) at the commit NOOK was built and tested against, if the folder is empty
# (a download of this repository as a zip has no submodules).
set -e
cd "$(dirname "$0")/.."
if [ ! -f fm1-base/tools/build.py ]; then
  rm -rf fm1-base
  git clone https://github.com/charlesvestal/fm1-base fm1-base
  git -C fm1-base checkout c8e05997ce970875038156d2bbd69e712eac1543
  git -C fm1-base submodule update --init --recursive
fi
echo "fm1-base: $(git -C fm1-base rev-parse --short HEAD)"
