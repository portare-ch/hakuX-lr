package com.rfandango.haku_x.ui.insignia

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.Insignia
import com.rfandango.haku_x.ui.common.OnboardingCard
import com.rfandango.haku_x.ui.common.OnboardingLead
import com.rfandango.haku_x.ui.common.OnboardingNote
import com.rfandango.haku_x.ui.common.SubScreen

private enum class Step(val title: Int) {
  INTRO(R.string.insignia_step_intro),
  CONNECTION(R.string.insignia_step_connection),
  REGISTER(R.string.insignia_step_register),
  ACCOUNT(R.string.insignia_step_account),
  READY(R.string.insignia_step_ready),
}

/** The steps done, in order: the first not done is where the wizard opens (the intro when none is). */
private fun firstStep(s: InsigniaStatus): Step {
  val done = listOf(s.network, s.registered, s.accounts.isNotEmpty())
  return when {
    done.none { it } -> Step.INTRO
    done.all { it } -> Step.READY
    else -> Step.entries[1 + done.indexOf(false)]
  }
}

/** Guides users through networking, console registration and account creation. */
@Composable
fun InsigniaWizard(
  onBack: () -> Unit,
  onLaunchDashboard: () -> Unit,
  vm: InsigniaViewModel = viewModel(),
) {
  val context = LocalContext.current
  val status by vm.status.collectAsStateWithLifecycle()
  var step by rememberSaveable { mutableIntStateOf(firstStep(vm.status.value).ordinal) }
  LifecycleResumeEffect(Unit) {
    vm.refresh()
    onPauseOrDispose {}
  }
  val current = Step.entries[step]
  // Resume the wizard when the dashboard exits.
  val launchDashboard = { AppPrefs(context).insigniaWizardResume = true; onLaunchDashboard() }

  SubScreen(
    title = stringResource(R.string.insignia_title),
    subtitle = stringResource(R.string.insignia_step_of, step + 1, Step.entries.size, stringResource(current.title)),
    onBack = onBack,
  ) { padding ->
    Column(Modifier.padding(padding).fillMaxSize()) {
      LinearProgressIndicator(progress = { (step + 1f) / Step.entries.size }, modifier = Modifier.fillMaxWidth())
      Column(
        Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
      ) {
        Column(Modifier.widthIn(max = 640.dp).fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
          Text(stringResource(current.title), style = MaterialTheme.typography.headlineSmall)
          when (current) {
            Step.INTRO -> IntroStep()
            Step.CONNECTION -> ConnectionStep(status, vm)
            Step.REGISTER -> RegisterStep(status, launchDashboard)
            Step.ACCOUNT -> AccountStep(status, launchDashboard)
            Step.READY -> ReadyStep(status)
          }
        }
      }
      Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
        if (step > 0) {
          TextButton(onClick = { step-- }) { Text(stringResource(R.string.insignia_back)) }
        }
        Spacer(Modifier.weight(1f))
        if (current == Step.READY) {
          Button(onClick = onBack) { Text(stringResource(R.string.insignia_done)) }
        } else {
          Button(onClick = { step++ }) {
            Text(stringResource(if (current == Step.INTRO) R.string.insignia_start else R.string.insignia_next))
          }
        }
      }
    }
  }
}

/** A step's state: done (with a check) or what's left. */
@Composable
private fun StepState(done: Boolean, doneText: String, todoText: String) {
  Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
    if (done) {
      Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = MaterialTheme.colorScheme.primary,
           modifier = Modifier.size(20.dp))
    }
    Text(
      if (done) doneText else todoText,
      style = MaterialTheme.typography.titleSmall,
      color = if (done) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
    )
  }
}

@Composable
private fun ColumnScope.IntroStep() {
  OnboardingLead(stringResource(R.string.insignia_intro))
  OnboardingCard(stringResource(R.string.insignia_intro_needs)) {
    Text(stringResource(R.string.insignia_intro_needs_list))
  }
  OnboardingNote(stringResource(R.string.insignia_intro_note))
}

@Composable
private fun ColumnScope.ConnectionStep(s: InsigniaStatus, vm: InsigniaViewModel) {
  OnboardingLead(stringResource(R.string.insignia_connection))
  StepState(s.network, stringResource(R.string.insignia_connection_done), stringResource(R.string.insignia_connection_todo))
  if (!s.network) {
    Button(onClick = vm::useInsignia) { Text(stringResource(R.string.insignia_connection_use)) }
  }
  OnboardingNote(stringResource(R.string.insignia_connection_note, AppPrefs.INSIGNIA_DNS))
}

@Composable
private fun ColumnScope.RegisterStep(s: InsigniaStatus, launchDashboard: () -> Unit) {
  OnboardingLead(stringResource(R.string.insignia_register))
  StepState(s.registered, stringResource(R.string.insignia_register_done), stringResource(R.string.insignia_register_todo))
  OnboardingCard(stringResource(R.string.insignia_register_run)) {
    Text(stringResource(R.string.insignia_register_how))
    Button(enabled = s.network, onClick = launchDashboard) { Text(stringResource(R.string.insignia_dashboard_start)) }
    if (!s.network) OnboardingNote(stringResource(R.string.insignia_needs_connection))
  }
}

@Composable
private fun ColumnScope.AccountStep(s: InsigniaStatus, launchDashboard: () -> Unit) {
  val uri = LocalUriHandler.current
  OnboardingLead(stringResource(R.string.insignia_account))
  StepState(
    s.accounts.isNotEmpty(),
    stringResource(R.string.insignia_account_done, s.accounts.joinToString(", ")),
    stringResource(R.string.insignia_account_todo),
  )
  OnboardingCard(stringResource(R.string.insignia_account_new)) {
    Text(stringResource(R.string.insignia_account_new_how))
    OutlinedButton(onClick = { uri.openUri(Insignia.SITE) }) { Text(stringResource(R.string.insignia_account_open_site)) }
  }
  OnboardingCard(stringResource(R.string.insignia_account_have)) {
    Text(stringResource(R.string.insignia_account_have_how))
  }
  Button(enabled = s.network && s.registered, onClick = launchDashboard) {
    Text(stringResource(R.string.insignia_dashboard_start))
  }
  if (!s.network || !s.registered) OnboardingNote(stringResource(R.string.insignia_account_needs))
}

@Composable
private fun ColumnScope.ReadyStep(s: InsigniaStatus) {
  val ready = s.network && s.registered && s.accounts.isNotEmpty()
  OnboardingLead(stringResource(if (ready) R.string.insignia_ready else R.string.insignia_ready_not))
  OnboardingCard(stringResource(R.string.insignia_ready_tips)) {
    Text(stringResource(R.string.insignia_ready_tips_text))
  }
}
