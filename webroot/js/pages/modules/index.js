import { exec, toast } from '../../kernelsu.js'

import { whichCurrentPage } from '../navbar.js'
import { getStrings } from '../pageLoader.js'

async function _getReZygiskState() {
  let stateCmd = await exec('/system/bin/cat /data/adb/rezygisk/state.json')
  if (stateCmd.errno !== 0) {
    toast('Error getting state of ReZygisk!')

    return;
  }

  try {
    const ReZygiskState = JSON.parse(stateCmd.stdout)
    return ReZygiskState
  } catch {
    return null;
  }
}

/* INFO: Text as HTML text: module names come from other modules' module.prop */
function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c])
}

/* INFO: Module ids as root managers accept them: anything else never reaches the shell */
const moduleIdRegex = /^[a-zA-Z][a-zA-Z0-9._-]+$/

async function _getModuleNames(modules) {
  /* INFO: Exactly one "N:<name>" line per module, in module order (empty name
             when it has none), so every name lands on its own module */
  const fullCommand = modules.map((mod) => {
    if (!moduleIdRegex.test(mod.id)) return `printf 'N:\\n'`

    const propPath = `/data/adb/modules/${mod.id}/module.prop`

    return `printf 'N:%s\\n' "$(/system/bin/grep -m 1 '^name=' "${propPath}" 2>/dev/null | /system/bin/cut -d '=' -f 2-)"`
  }).join(' ; ')

  const result = await exec(fullCommand)
  if (result.errno !== 0) {
    toast('Failed to execute command to retrieve module list names')

    return null
  }

  return result.stdout.split('\n').filter((line) => line.startsWith('N:')).map((line) => line.slice(2))
}

async function _updateDynamicElement() {
  const ReZygiskState = await _getReZygiskState()
  /* INFO: state.json unreadable or invalid: nothing to list */
  if (!ReZygiskState) return
  const all_modules = []
  const strings = await getStrings(whichCurrentPage())

  if (ReZygiskState.rezygiskd) Object.keys(ReZygiskState.rezygiskd).forEach((daemon_bit) => {
    const daemon = ReZygiskState.rezygiskd[daemon_bit]

    if (daemon.modules && daemon.modules.length > 0) {
      daemon.modules.forEach((module_id) => {
        const module = all_modules.find((mod) => mod.id === module_id)
        if (module) {
          module.bitsUsed.push(daemon_bit)
        } else {
          all_modules.push({
            id: module_id,
            name: null,
            bitsUsed: [ daemon_bit ]
          })
        }
      })
    }
  })

  if (all_modules.length !== 0) {
    const modules_list = document.getElementById('modules_list')
    modules_list.innerHTML = `
      <div id="modules_list_not_avaliable" class="not_avaliable">
        ${strings.notAvaliable}
      </div>
    `
    document.getElementById('modules_list_not_avaliable').style.display = 'none'

    const module_names = await _getModuleNames(all_modules)
    if (module_names) module_names.forEach((module_name, i) => all_modules[i].name = module_name)

    all_modules.forEach((module) => {
      modules_list.innerHTML +=
        `<div class="dim card" style="padding: 25px 15px; cursor: pointer;">
          <div class="dimc" style="font-size: 1.1em;">${escapeHtml(module.name || module.id)}</div>
          <div class="dimc desc" style="font-size: 0.9em; margin-top: 3px; white-space: nowrap; align-items: center; display: flex;">
            <div class="dimc arch_desc">${strings.arch}</div>
            <div class="dimc" style="margin-left: 5px;">${module.bitsUsed.join(' / ')}</div>
          </div>
        </div>`
    })
  }
}

export async function loadOnce() {

}

export async function loadOnceView() {
  _updateDynamicElement()
}

export async function onceViewAfterUpdate() {
  _updateDynamicElement()
}

export async function load() {

}
