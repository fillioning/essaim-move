#!/bin/bash
set -e
MODULE_ID="essaim"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators/$MODULE_ID"

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh root@$MOVE_HOST "mkdir -p $DEST"
scp "dist/$MODULE_ID/dsp.so" "dist/$MODULE_ID/module.json" "root@$MOVE_HOST:$DEST/"
ssh root@$MOVE_HOST "chmod +x $DEST/dsp.so && chown -R ableton:users $DEST"
echo "Done. Remove and re-add the module on Move to reload."
