package com.rfandango.haku_x.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SwitchItem

/**
 * The console's network cable (native/net), as on a real console: plugged in, Xbox Live through Insignia (the
 * Internet through NAT, its DNS server or an insigniaDNS one) and system link with the consoles on this network
 * at once, others reached directly too (another subnet, a VPN); or not. Read at the next game start.
 */
@Composable
fun NetworkSettings(prefs: AppPrefs, onInsignia: () -> Unit) {
  var on by remember { mutableStateOf(prefs.networkOn) }
  var peers by remember { mutableStateOf(prefs.lanPeers) }
  var dns by remember { mutableStateOf(prefs.insigniaDns) }
  var peersDialog by remember { mutableStateOf(false) }
  var dnsDialog by remember { mutableStateOf(false) }
  // back from Insignia's setup, which may have plugged the cable in
  LifecycleResumeEffect(Unit) {
    on = prefs.networkOn
    onPauseOrDispose {}
  }

  Section(stringResource(R.string.settings_section_network))
  Hint(stringResource(R.string.settings_network_desc))
  // Insignia's setup wizard: everything online play needs, step by step
  ListItem(
    headlineContent = { Text(stringResource(R.string.insignia_title)) },
    supportingContent = { Text(stringResource(R.string.insignia_settings_desc)) },
    modifier = Modifier.clickable(onClick = onInsignia),
  )
  SwitchItem(
    title = stringResource(R.string.settings_network_cable),
    summary = stringResource(R.string.settings_network_cable_desc) + " " +
      stringResource(R.string.settings_applies_next_start),
    checked = on,
    onChange = { on = it; prefs.networkOn = it },
  )
  if (on) {
    ListItem(
      headlineContent = { Text(stringResource(R.string.settings_network_insignia_dns)) },
      supportingContent = {
        Text(dns.ifBlank { stringResource(R.string.settings_network_insignia_dns_default, AppPrefs.INSIGNIA_DNS) } +
          ". " + stringResource(R.string.settings_network_insignia_dns_desc))
      },
      modifier = Modifier.clickable { dnsDialog = true },
    )
    ListItem(
      headlineContent = { Text(stringResource(R.string.settings_network_peers)) },
      supportingContent = {
        Text(peers.ifBlank { stringResource(R.string.settings_network_peers_none) } + ". " +
          stringResource(R.string.settings_network_peers_desc))
      },
      modifier = Modifier.clickable { peersDialog = true },
    )
  }

  if (peersDialog) {
    TextDialog(
      title = stringResource(R.string.settings_network_peers),
      hint = stringResource(R.string.settings_network_peers_hint),
      value = peers,
      onDone = { peers = it.trim(); prefs.lanPeers = peers; peersDialog = false },
      onDismiss = { peersDialog = false },
    )
  }
  if (dnsDialog) {
    TextDialog(
      title = stringResource(R.string.settings_network_insignia_dns),
      hint = AppPrefs.INSIGNIA_DNS,
      value = dns,
      onDone = { dns = it.trim(); prefs.insigniaDns = dns; dnsDialog = false },
      onDismiss = { dnsDialog = false },
    )
  }
}

/** One line of text to edit (addresses). */
@Composable
private fun TextDialog(title: String, hint: String, value: String, onDone: (String) -> Unit, onDismiss: () -> Unit) {
  var text by remember { mutableStateOf(value) }
  AlertDialog(
    onDismissRequest = onDismiss,
    title = { Text(title) },
    text = {
      OutlinedTextField(
        value = text,
        onValueChange = { text = it },
        singleLine = true,
        placeholder = { Text(hint) },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
      )
    },
    confirmButton = { TextButton(onClick = { onDone(text) }) { Text(stringResource(android.R.string.ok)) } },
    dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.cancel)) } },
  )
}
