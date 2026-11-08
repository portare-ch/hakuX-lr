package com.rfandango.haku_x.data.hdd

/**
 * The dashboard: hakuX-lr's own, which the core puts on C: (Partition2) as xboxdash.xbe and
 * xodash\xonlinedash.xbe at every boot, replacing any other dashboard there (native/core/dash.c).
 */
object Dashboard {
  /** NativeCoreActivity's disc for the dashboard: none, the core booting C:\xboxdash.xbe. */
  const val URI = "hakux:dashboard"
}
