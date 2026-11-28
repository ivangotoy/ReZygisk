import { whichCurrentPage } from '../navbar.js'
import { getStrings } from '../pageLoader.js'
import { exec, toast } from '../../kernelsu.js'
import utils from '../utils.js'

/* INFO: 32-bit-only devices have only zygisk-ptrace32; either one reaches the monitor */
function monitorCtl(command) {
  return exec(`for abi in 64 32; do tracer=/data/adb/modules/rezygisk/bin/zygisk-ptrace$abi; if [ -x "$tracer" ]; then exec "$tracer" ctl ${command}; fi; done; exit 1`)
}

async function _getMonitorState() {
  const stateCmd = await exec('/system/bin/cat /data/adb/rezygisk/state.json')
  if (stateCmd.errno !== 0) {
    toast('Error getting state of ReZygisk!')

    return;
  }

  try {
    const ReZygiskState = JSON.parse(stateCmd.stdout)
    return ReZygiskState.monitor.state
  } catch {
    return null;
  }
}

async function _updateDynamicElement() {
  const monitor_status = document.getElementById('monitor_status')
  const strings = await getStrings(whichCurrentPage())
  const monitorState = await _getMonitorState()

  if (monitorState == null) return;

  switch (monitorState) {
    case '0': monitor_status.innerHTML = strings.monitor.status.tracing; break;
    case '1': monitor_status.innerHTML = strings.monitor.status.stopping; break;
    case '2': monitor_status.innerHTML = strings.monitor.status.stopped; break;
    case '3': monitor_status.innerHTML = strings.monitor.status.exiting; break;
    default: monitor_status.innerHTML = strings.monitor.status.unknown;
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
  const monitor_start = document.getElementById('monitor_start_button')
  const monitor_stop = document.getElementById('monitor_stop_button')
  const monitor_pause = document.getElementById('monitor_pause_button')
  const monitor_status = document.getElementById('monitor_status')
  const strings = await getStrings(whichCurrentPage())

  utils.addListener(monitor_start, 'click', () => {
    if (![ strings.monitor.status.tracing, strings.monitor.status.stopping, strings.monitor.status.stopped ].includes(monitor_status.innerHTML)) return;
    monitor_status.innerHTML = strings.monitor.status.tracing
    monitorCtl('start')
  })

  utils.addListener(monitor_stop, 'click', () => {
    monitor_status.innerHTML = strings.monitor.status.exiting
    monitorCtl('exit')
  })

  utils.addListener(monitor_pause, 'click', () => {
    if (![ strings.monitor.status.tracing, strings.monitor.status.stopping, strings.monitor.status.stopped ].includes(monitor_status.innerHTML)) return;
    monitor_status.innerHTML = strings.monitor.status.stopped
    monitorCtl('stop')
  })

  return;
}