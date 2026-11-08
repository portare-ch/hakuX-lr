package com.rfandango.haku_x.ui.insignia

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.Insignia
import com.rfandango.haku_x.data.hdd.HddStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/** What Insignia's setup has done: each step's state, read from the settings and the hard disk. */
data class InsigniaStatus(
  /** The console's network cable is plugged in (AppPrefs.networkOn). */
  val network: Boolean = false,
  /** The dashboard registered the console (its machine credential is in the config area). */
  val registered: Boolean = false,
  /** The Insignia accounts on the console (gamertags). */
  val accounts: List<String> = emptyList(),
)

/** Loads the setup wizard's state from preferences and disk. */
class InsigniaViewModel(app: Application) : AndroidViewModel(app) {
  private val prefs = AppPrefs(app)
  private val hddRoot = HddStore(app).root

  private val _status = MutableStateFlow(read())
  val status: StateFlow<InsigniaStatus> = _status

  init {
    viewModelScope.launch(Dispatchers.IO) { Insignia.forgetSetupAssistant(app) }
  }

  private fun read() = InsigniaStatus(
    network = prefs.networkOn,
    registered = Insignia.registered(hddRoot),
    accounts = Insignia.accounts(hddRoot),
  )

  /** Refresh after returning from the dashboard or settings. */
  fun refresh() {
    _status.value = read()
  }

  fun useInsignia() {
    prefs.networkOn = true
    refresh()
  }
}
