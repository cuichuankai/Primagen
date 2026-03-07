#!/bin/sh

WORK_DIR=$(pwd)
if [ ! -z "$1" ]; then
    WORK_DIR=$1
    mkdir -p $CONFDIR
fi

# extract
ARCHIVE_LINE=$(awk '/^__ARCHIVE_BELOW__/ {print NR + 1; exit 0; }' "$0")
tail -n +$ARCHIVE_LINE "$0" | tar -xz -C "$WORK_DIR"
cd $WORK_DIR
./primagen onboard
cd -
exit 0
__ARCHIVE_BELOW__
