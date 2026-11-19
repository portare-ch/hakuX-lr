#!/system/bin/sh
# Boot each disc image with xboxrun to its first kernel call. Print the frontier and
# kernel-import lines for tools/native/imports.py. Deploy beside xboxrun under
# /data/local/tmp/native. Exit 4 if the app is running before a title starts.
#
# sh census.sh [/sdcard/ROMs/xbox]
app_running() {
  pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
    dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x
}
cd /data/local/tmp/native || exit 1
chmod 755 xboxrun
DIR=${1:-/sdcard/ROMs/xbox}
for F in "$DIR"/*.iso; do
  if app_running; then
    echo "hakuX app is running: stopping (device runs must not overlap it)"
    exit 4
  fi
  echo "== $F"
  timeout 60 ./xboxrun "$F" 2>&1 | grep -E 'hakuX-lr-native-(frontier|imports)'
done
