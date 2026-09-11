#!/usr/bin/env bash
set -u
. /tmp/wp02/env.sh
for i in 1 2 3 4 5; do
  /tmp/wp02/build/capture screenshot -o "/tmp/wp02/out/shot$i.png" 2>&1 \
    | grep -E 'screenshot_response|screenshot_saved'
done
identify /tmp/wp02/out/shot5.png
convert /tmp/wp02/out/shot5.png -format 'shot5 mean=%[mean] stddev=%[standard-deviation] colors=%k\n' info:
