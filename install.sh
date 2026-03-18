#!/bin/sh

# Colors for output
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
PINK='\033[1;35m'
NC='\033[0m' # No Color

echo "${BLUE}"
echo "_____________________________  _________________________________   __"
echo "___  __ \__  __ \___  _/__   |/  /__    |___  ____/__  ____/__  | / /"
echo "__  /_/ /_  /_/ /__  / __  /|_/ /__  /| |__  / __ __  __/  __   |/ / "
echo "_  ____/_  _, _/__/ /  _  /  / / _  ___ | / /_/ / _  /___  _  /|  /  "
echo "/_/     /_/ |_| /___/  /_/  /_/  /_/  |_| \____/  /_____/  /_/ |_/   "
echo "${NC}"
echo "${YELLOW}Start install ...${NC}"


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
echo "${GREEN}Installation successful.${NC}"
exit 0
__ARCHIVE_BELOW__
