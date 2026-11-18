// Host tests for the WebUI code that builds root shell commands.
//   node tests/webui/webui_test.mjs <case> <scratch dir>
// The real webroot/js files are copied into the scratch dir (with a
// package.json marking them as ES modules) and imported with browser
// and KernelSU stand-ins: ksu.exec records every command it is given.
import { cpSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const [, , testCase, scratch] = process.argv
const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const web = join(scratch, 'webroot')
mkdirSync(web, { recursive: true })
cpSync(join(root, 'webroot', 'js'), join(web, 'js'), { recursive: true })
cpSync(join(root, 'webroot', 'lang'), join(web, 'lang'), { recursive: true })
writeFileSync(join(web, 'package.json'), '{"type":"module"}\n')

const rejections = []
process.on('unhandledRejection', (e) => rejections.push(e))

// anything: a value every DOM access works on
const anything = new Proxy(function () {}, {
  get: (t, p) => (p === Symbol.toPrimitive ? () => '' : p === 'then' ? undefined : anything),
  set: () => true,
  apply: () => anything
})

const handlers = {}
const commands = []
let stateJson = '{}'
let moduleNamesStdout = ''
let failing = null

globalThis.window = new Proxy({
  addEventListener: (name, fn) => { (handlers[name] ||= []).push(fn) },
  onceTrueEvent: () => {}
}, {
  get: (t, p) => (p in t ? t[p] : p in globalThis ? globalThis[p] : anything),
  set: (t, p, v) => { t[p] = v; globalThis[p] = v; return true }
})
// document: getElementById gives elements that keep their innerHTML,
// addEventListener records the handlers
const elements = {}
const documentHandlers = {}
const element = (id) => (elements[id] ||= new Proxy({ innerHTML: '' }, {
  get: (t, p) => (p === 'innerHTML' ? t.innerHTML : anything),
  set: (t, p, v) => { if (p === 'innerHTML') t.innerHTML = String(v); return true }
}))
globalThis.document = new Proxy({}, {
  get: (t, p) => (p === 'getElementById' ? element :
    p === 'addEventListener' ? (name, fn) => { (documentHandlers[name] ||= []).push(fn) } : anything)
})
globalThis.getComputedStyle = () => anything
globalThis.requestAnimationFrame = () => 0
globalThis.screen = { height: 0 }
// browser classes that webroot/js/pages/utils.js extends
for (const name of ['Window', 'Element', 'HTMLElement', 'NodeList', 'HTMLCollection', 'Document', 'EventTarget']) {
  globalThis[name] ||= class {}
}
globalThis.localStorage = { getItem: () => null, setItem: () => {} }
globalThis.fetch = async (url) => {
  const text = readFileSync(join(web, String(url)), 'utf8')

  return { json: async () => JSON.parse(text), text: async () => text }
}
globalThis.ksu = {
  exec: (command, options, callback) => {
    commands.push(command)
    const stdout = command.includes('state.json') ? stateJson : command.includes('module.prop') ? moduleNamesStdout : ''
    const errno = failing && command.includes(failing) ? 1 : 0
    setTimeout(() => globalThis[callback](errno, errno ? '' : stdout, ''), 0)
  },
  toast: () => {},
  fullScreen: () => {}
}

const pageLoader = await import(pathToFileURL(join(web, 'js', 'pages', 'pageLoader.js')))
const settle = () => new Promise((resolve) => setTimeout(resolve, 50))

// sh -c on the command, with the redirect target swapped for a file in the scratch dir
const { execFileSync } = await import('node:child_process')
const runShell = (command, out) => execFileSync('sh', ['-c', command.replace('/data/adb/rezygisk/webui_error.log', out)])

function fail(message) {
  console.log(message)
  process.exit(1)
}

async function errorLog(kind, event, wantText) {
  await settle()
  commands.length = 0
  for (const fn of handlers[kind] || []) fn(event)
  await settle()

  const command = commands.find((c) => c.includes('webui_error.log'))
  if (!command) fail(`no log command for ${kind}`)

  const quoted = `'${wantText.replace(/'/g, "'\\''")}'`
  const wantCommand = `printf '%s\\n' ${quoted} > /data/adb/rezygisk/webui_error.log`
  if (command !== wantCommand) fail(`command:  ${command}\nexpected: ${wantCommand}`)

  const out = join(scratch, `${kind}.log`)
  try {
    runShell(command, out)
  } catch (e) {
    fail(`command failed: ${command}`)
  }
  const got = readFileSync(out, 'utf8')
  if (got !== wantText + '\n') fail(`command: ${command}\nlogged:   ${JSON.stringify(got)}\nexpected: ${JSON.stringify(wantText + '\n')}`)
}

const nasty = `it's "quoted" $(touch ${scratch}/pwned) \`touch ${scratch}/pwned\` back\\slash\nline two`

switch (testCase) {
  case 'error_log_literal': {
    const stack = `Error: ${nasty}\n    at load (js/pages/home/index.js:1:1)`
    await errorLog('error', { message: nasty, error: { stack } }, `Error: ${nasty}\n\n${stack}`)
    break
  }
  case 'error_log_no_error_object': {
    await errorLog('error', { message: 'Script error.', error: null }, 'Error: Script error.\n\n')
    break
  }
  case 'rejection_log_literal': {
    await errorLog('unhandledrejection', { reason: nasty }, `Error (Unhandled Rejection): ${nasty}\n\n`)
    break
  }
  case 'module_ids_validated': {
    stateJson = JSON.stringify({
      rezygiskd: { 64: { modules: ['example_module', `bad"; touch ${scratch}/pwned; "`, 'other.module-2'] } }
    })
    const modules = await import(pathToFileURL(join(web, 'js', 'pages', 'modules', 'index.js')))
    commands.length = 0
    await modules.loadOnceView()
    await settle()

    const command = commands.find((c) => c.includes('module.prop'))
    if (!command) fail('no module name command')
    if (command.includes('pwned') || !command.includes('/data/adb/modules/example_module/module.prop') ||
        !command.includes('/data/adb/modules/other.module-2/module.prop')) fail(`command: ${command}`)
    // one name slot per module, in order: each slot ends with printf "\n"
    const slots = command.split('printf "\\n"').length - 1
    if (slots !== 3) fail(`${slots} name slots for 3 modules: ${command}`)
    break
  }
  case 'module_names_escaped': {
    stateJson = JSON.stringify({ rezygiskd: { 64: { modules: ['example_module'] } } })
    moduleNamesStdout = `<img src=x onerror="ksu.exec('reboot')"> & Example`
    const modules = await import(pathToFileURL(join(web, 'js', 'pages', 'modules', 'index.js')))
    await modules.loadOnceView()
    await settle()

    const html = elements.modules_list ? elements.modules_list.innerHTML : ''
    if (!html.includes('&lt;img src=x onerror=&quot;ksu.exec(&#39;reboot&#39;)&quot;&gt; &amp; Example') || html.includes('<img')) fail(`modules_list: ${html}`)
    break
  }
  case 'modules_state_unreadable': {
    failing = 'state.json'
    const modules = await import(pathToFileURL(join(web, 'js', 'pages', 'modules', 'index.js')))
    rejections.length = 0
    await modules.loadOnceView()
    await settle()
    if (rejections.length) fail(`page update failed: ${rejections[0]}`)
    break
  }
  case 'modules_names_unreadable': {
    stateJson = JSON.stringify({ rezygiskd: { 64: { modules: ['example_module'] } } })
    failing = 'module.prop'
    const modules = await import(pathToFileURL(join(web, 'js', 'pages', 'modules', 'index.js')))
    rejections.length = 0
    await modules.loadOnceView()
    await settle()
    if (rejections.length) fail(`page update failed: ${rejections[0]}`)
    const html = elements.modules_list ? elements.modules_list.innerHTML : ''
    if (!html.includes('example_module')) fail(`modules_list without the module: ${html}`)
    break
  }
  case 'credit_link_quoted': {
    await import(pathToFileURL(join(web, 'js', 'preloader.js')))
    await settle()
    commands.length = 0
    const link = `github.com/example'; touch ${scratch}/pwned; echo '`
    for (const fn of documentHandlers.click || []) await fn({ target: { getAttribute: () => link } })
    await settle()

    const want = `am start -a android.intent.action.VIEW -d 'https://${link.replace(/'/g, "'\\''")}'`
    const command = commands.find((c) => c.startsWith('am start'))
    if (command !== want) fail(`command:  ${command}\nexpected: ${want}`)
    break
  }
  default:
    process.exit(2)
}

process.exit(0)
