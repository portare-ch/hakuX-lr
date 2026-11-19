#!/system/bin/sh
# Run FEXCore tests on the device. Place this script beside fexrun in /data/local/tmp/fexrun,
# with the upstream suite under tests/ and local cases under sysops/.
#
# sh fexrun-suite.sh identity|auto|diff|sysops
#
# identity uses host addresses for guest memory. auto uses the memory-base register. diff
# compares CPU state and guest pages between them. sysops runs both suites with
# system-instruction exits enabled.
#
# Run at low priority on the small cores. Exit 4 if the app is running. Print each run's
# summary and non-passing cases.
app_running() {
  pidof com.rfandango.haku_x.debug:native >/dev/null || pidof com.rfandango.haku_x >/dev/null ||
    dumpsys window | grep -m1 mCurrentFocus | grep -q haku_x
}
cd /data/local/tmp/fexrun || exit 1
chmod 755 fexrun
MODE=${1:-identity}
RUN="taskset 3f nice -n 10 ./fexrun"
for CFG in "1 0" "500 0" "500 1"; do
  if app_running; then
    echo "hakuX app is running: stopping (device runs must not overlap it)"
    exit 4
  fi
  set -- $CFG
  JIT="--maxinst $1 --multiblock $2"
  OUT=suite_${MODE}_$1_$2
  case $MODE in
  diff)
    $RUN --diff $JIT tests tests/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep '^DIFF' $OUT.txt | grep -v gather
    ;;
  sysops)
    $RUN --sysops --diff $JIT sysops sysops/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep -v '^SAME [^ ]* PASS/PASS' $OUT.txt | grep -v '^summary'
    $RUN --sysops --diff $JIT tests tests/manifest.txt > ${OUT}_fex.txt 2> ${OUT}_fex.err
    tail -n 1 ${OUT}_fex.txt
    grep '^DIFF' ${OUT}_fex.txt | grep -v gather
    ;;
  *)
    $RUN --base $MODE $JIT tests tests/manifest.txt > $OUT.txt 2> $OUT.err
    tail -n 1 $OUT.txt
    grep -v '^PASS' $OUT.txt | grep -v '^summary' | grep -v gather
    ;;
  esac
done
