#!/bin/sh

# Keep HomeMind's bundled libraries ahead of the read-only Atlas system image.
LD_LIBRARY_PATH=/data/share/homemind/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export LD_LIBRARY_PATH

exec /data/share/homemind/bin/mosquitto.real "$@"
