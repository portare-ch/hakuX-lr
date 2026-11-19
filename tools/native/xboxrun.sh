#!/system/bin/sh
# Run one title on the device from /data/local/tmp/native. Exit 4 if the emulator process or
# release app is running, or hakuX is foregrounded. Android may keep the debug UI cached in
# the background.
#
# sh xboxrun.sh <disc image> [option...]
#
# See native/run/xboxrun.c for options.
if pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
   dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x; then
  echo "hakuX app is running: not starting (device runs must not overlap it)"
  exit 4
fi
cd /data/local/tmp/native || exit 1
chmod 755 xboxrun
exec ./xboxrun "$@"
