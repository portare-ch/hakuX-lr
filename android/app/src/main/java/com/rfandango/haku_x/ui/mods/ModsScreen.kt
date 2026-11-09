package com.rfandango.haku_x.ui.mods

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.ModCatalog
import com.rfandango.haku_x.data.ModChoices
import com.rfandango.haku_x.ui.common.EmptyText
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SubScreen
import com.rfandango.haku_x.ui.common.SwitchItem

/**
 * One disc's mods and cheats (the repository's mods/ folder): each on or
 * off. Mods take the next start; cheats also turn on and off in the game's
 * menu while it runs.
 */
@Composable
fun ModsScreen(gameKey: String, title: String, onBack: () -> Unit) {
  val context = LocalContext.current
  val game = remember(gameKey) { ModCatalog.load(context, gameKey) }
  val choices = remember(gameKey) { ModChoices(context, gameKey) }
  val mods = remember(gameKey) { mutableStateMapOf<String, Boolean>().apply { game.mods.forEach { put(it.id, choices.mod(it.id)) } } }
  val cheats = remember(gameKey) {
    mutableStateMapOf<String, Boolean>().apply { game.cheats.forEach { put(it.name, choices.cheat(it.name)) } }
  }

  val allowed = remember { AppPrefs(context).cheats }

  SubScreen(title = stringResource(R.string.mods_title), subtitle = title, onBack = onBack) { padding ->
    Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState())) {
      if (!game.isEmpty) {
        Hint(stringResource(if (allowed) R.string.mods_online_note else R.string.mods_off_note))
      }
      if (game.isEmpty) {
        EmptyText(stringResource(R.string.mods_none))
      }
      if (game.mods.isNotEmpty()) {
        Section(stringResource(R.string.mods_section_mods))
        Hint(stringResource(R.string.mods_mods_desc))
        for (mod in game.mods) {
          val by = if (mod.author.isNotEmpty()) stringResource(R.string.mods_by, mod.author) else ""
          SwitchItem(
            title = mod.name,
            summary = listOf(mod.description, by).filter { it.isNotEmpty() }.joinToString("\n").ifEmpty { null },
            checked = mods[mod.id] == true,
            onChange = { v ->
              choices.setMod(mod.id, v)
              mods[mod.id] = v
            },
          )
        }
      }
      if (game.cheats.isNotEmpty()) {
        Section(stringResource(R.string.mods_section_cheats))
        Hint(stringResource(R.string.mods_cheats_desc))
        for (cheat in game.cheats) {
          SwitchItem(
            title = cheat.name,
            summary = cheat.note.ifEmpty { null },
            checked = cheats[cheat.name] == true,
            onChange = { v ->
              choices.setCheat(cheat.name, v)
              cheats[cheat.name] = v
            },
          )
        }
      }
    }
  }
}
