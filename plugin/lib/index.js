// local-root-agent — dsh-go 控制台：查看并调用 Android 侧的 Root Agent 守护进程。
// 守护进程由 KernelSU/SukiSU 模块（Root Agent）在开机时以真 root 启动，只监听 127.0.0.1。
// 1.2.0：引擎启动时做一次「升级自检」——App 升级会重写 ~/.dsh/AGENTS.md，这里自动修正它，
// 顺带恢复 dsh-go 侧的 client / shim；profile、令牌、守护进程只在面板里提示（改动需要重启引擎或点模块）。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

export const name = 'local-root-agent'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const PAIRS_FILE = path.join(HERE, 'agents-pairs.txt')
const SECTION_FILE = path.join(HERE, 'agents-section.md')

const PORT = Number(process.env.DSH_ROOT_AGENT_PORT || 47831) || 47831
const BASE = 'http://127.0.0.1:' + PORT
const HEALTH_TIMEOUT = 1200

const TOKEN_FILE = process.env.DSH_RA_TOKEN || '/root/.dsh/rootagent-token'
const AGENTS = process.env.DSH_RA_AGENTS || '/root/.dsh/AGENTS.md'
const PROFILE = process.env.DSH_RA_PROFILE || '/root/.dsh/profiles/web/cordis.patch.yml'
const BIN_DIR = process.env.DSH_RA_BIN || '/usr/local/bin'
const CLIENT = path.join(BIN_DIR, 'dshsb-client')
const SHIM = path.join(BIN_DIR, 'dsh-bwrap-root')
const ROOTRUN = path.join(BIN_DIR, 'rootrun')
// 可选：项目的源码/产物目录（装 client/shim/rootrun 时用）。留空则只检查不自动安装。
const SRC = process.env.DSH_RA_SRC || ''
const MARK = '本机 root 通道与沙箱'

const PROFILE_BLOCK = [
  '# dsh-go 沙箱后端：root 侧 mount-namespace runner',
  '- id: sandbox',
  '  name: "@deepseek-ai/dsh-sandbox-local"',
  '  config:',
  '    runnerCommand:',
  '      - /usr/local/bin/dsh-bwrap-root',
  '    runnerFailureSignatures:',
  '      - "dsh-bwrap-root: "',
].join('\n')

const cache = {
  online: false,
  pid: 0,
  uptimeMs: 0,
  requests: 0,
  note: '还没连过',
  at: 0,
}

const selfTest = { at: 0, ok: false, text: '还没跑过自检' }

const checkState = { at: 0, items: [], boot: null }

function readText(file) {
  try {
    return fs.readFileSync(file, 'utf8')
  } catch {
    return null
  }
}

function readToken() {
  const s = readText(TOKEN_FILE)
  return s ? s.trim() : ''
}

function mask(token) {
  return token ? token.slice(0, 4) + '…' + token.slice(-4) : '读不到令牌文件'
}

function writeAtomic(file, text) {
  let mode = 0o644
  try {
    mode = fs.statSync(file).mode & 0o777
  } catch {
    mode = 0o644
  }
  const tmp = file + '.tmp-' + process.pid
  fs.writeFileSync(tmp, text, { mode })
  try {
    fs.chmodSync(tmp, mode)
  } catch {}
  fs.renameSync(tmp, file)
}

function readPairs() {
  const raw = readText(PAIRS_FILE) || ''
  const out = []
  for (const line of raw.split('\n')) {
    if (line.indexOf('|@@@|') < 0) continue
    const at = line.indexOf('|@@@|')
    out.push([line.slice(0, at), line.slice(at + 5)])
  }
  return out
}

function checkAgents() {
  const text = readText(AGENTS)
  if (text == null) {
    return { id: 'agents', label: '托管说明 AGENTS.md', state: 'bad', detail: '文件不存在' }
  }
  const stale = readPairs().filter((pair) => pair[0] && text.includes(pair[0])).length
  const hasSection = text.includes(MARK)
  if (stale || !hasSection) {
    const bits = []
    if (stale) bits.push(stale + ' 行过时说明')
    if (!hasSection) bits.push('缺 root 通道段落')
    return { id: 'agents', label: '托管说明 AGENTS.md', state: 'warn', detail: bits.join('，') }
  }
  return { id: 'agents', label: '托管说明 AGENTS.md', state: 'ok', detail: '已是最新' }
}

function checkClient() {
  try {
    const st = fs.statSync(CLIENT)
    if (st.size > 0) return { id: 'client', label: '沙箱 client', state: 'ok', detail: '在 ' + CLIENT }
  } catch {}
  return { id: 'client', label: '沙箱 client', state: 'warn', detail: '缺 ' + CLIENT }
}

function checkShim() {
  try {
    const st = fs.lstatSync(SHIM)
    if (st.isSymbolicLink()) {
      const target = fs.realpathSync(SHIM)
      if (fs.existsSync(target)) return { id: 'shim', label: 'bwrap 方言 shim', state: 'ok', detail: '指向 ' + target }
      return { id: 'shim', label: 'bwrap 方言 shim', state: 'warn', detail: '软链目标不存在' }
    }
    return { id: 'shim', label: 'bwrap 方言 shim', state: 'ok', detail: '在 ' + SHIM }
  } catch {}
  return { id: 'shim', label: 'bwrap 方言 shim', state: 'warn', detail: '缺 ' + SHIM }
}

function checkRootrun() {
  try {
    const st = fs.lstatSync(ROOTRUN)
    if (st.isSymbolicLink()) {
      const target = fs.realpathSync(ROOTRUN)
      if (fs.existsSync(target)) return { id: 'rootrun', label: 'rootrun 命令', state: 'ok', detail: '指向 ' + target }
      return { id: 'rootrun', label: 'rootrun 命令', state: 'warn', detail: '软链目标不存在' }
    }
    return { id: 'rootrun', label: 'rootrun 命令', state: 'ok', detail: '在 ' + ROOTRUN }
  } catch {}
  return { id: 'rootrun', label: 'rootrun 命令', state: 'warn', detail: '缺 ' + ROOTRUN }
}

function checkProfile() {
  const text = readText(PROFILE)
  if (text == null) return { id: 'profile', label: '沙箱 runnerCommand', state: 'bad', detail: '读不到 ' + PROFILE }
  if (text.includes('runnerCommand')) return { id: 'profile', label: '沙箱 runnerCommand', state: 'ok', detail: '已配置' }
  if (text.includes('id: sandbox')) {
    return { id: 'profile', label: '沙箱 runnerCommand', state: 'warn', detail: 'sandbox 条目缺 runnerCommand，需手工补' }
  }
  return { id: 'profile', label: '沙箱 runnerCommand', state: 'warn', detail: '缺 runnerCommand，点「补配置」' }
}

function checkToken() {
  try {
    const st = fs.statSync(TOKEN_FILE)
    if (st.size >= 32) return { id: 'token', label: 'root 令牌', state: 'ok', detail: st.size + ' 字节' }
    return { id: 'token', label: 'root 令牌', state: 'warn', detail: '文件过短（' + st.size + ' 字节）' }
  } catch {}
  return { id: 'token', label: 'root 令牌', state: 'bad', detail: '缺 ' + TOKEN_FILE }
}

function checkDaemon() {
  if (cache.online) return { id: 'daemon', label: '守护进程', state: 'ok', detail: 'pid ' + cache.pid + '，运行 ' + duration(cache.uptimeMs) }
  return { id: 'daemon', label: '守护进程', state: 'bad', detail: '连不上 127.0.0.1:' + PORT + '，SukiSU 点模块「执行」' }
}

function runCheck() {
  checkState.items = [checkAgents(), checkClient(), checkShim(), checkRootrun(), checkProfile(), checkToken(), checkDaemon()]
  checkState.at = Date.now()
  return checkState.items
}

function ensureAgents() {
  const text = readText(AGENTS)
  if (text == null) return null
  let out = text
  const changed = []
  for (const pair of readPairs()) {
    const old = pair[0]
    const now = pair[1]
    if (!old || !now) continue
    if (!out.includes(old)) continue
    out = out.split(old).join(now)
    changed.push('过时说明')
  }
  if (!out.includes(MARK)) {
    const section = readText(SECTION_FILE)
    if (section) {
      out = out.replace(/\s+$/, '') + '\n' + section
      changed.push('root 通道段落')
    }
  }
  if (!changed.length) return null
  writeAtomic(AGENTS, out)
  const uniq = changed.filter((v, i) => changed.indexOf(v) === i)
  return 'AGENTS.md（' + uniq.join('、') + '）'
}

function ensureClient() {
  if (!SRC) return null
  try {
    const st = fs.statSync(CLIENT)
    if (st.size > 0) return null
  } catch {}
  const cands = [path.join(SRC, 'module', 'bin', 'dshsb-client'), path.join(SRC, 'src', 'dshsb-client')]
  for (const cand of cands) {
    try {
      fs.copyFileSync(cand, CLIENT)
      fs.chmodSync(CLIENT, 0o755)
      return '沙箱 client'
    } catch {}
  }
  return null
}

function ensureShim() {
  if (!SRC) return null
  const src = path.join(SRC, 'src', 'dsh-bwrap-root')
  let present = false
  try {
    const st = fs.lstatSync(SHIM)
    present = st.isSymbolicLink() ? fs.existsSync(fs.realpathSync(SHIM)) : true
  } catch {}
  if (present) {
    try {
      fs.chmodSync(src, 0o755)
    } catch {}
    return null
  }
  try {
    fs.rmSync(SHIM, { force: true })
    fs.symlinkSync(src, SHIM)
    fs.chmodSync(src, 0o755)
    return 'shim 软链'
  } catch {
    return null
  }
}

function ensureRootrun() {
  if (!SRC) return null
  const src = path.join(SRC, 'src', 'rootrun')
  let present = false
  try {
    const st = fs.lstatSync(ROOTRUN)
    present = st.isSymbolicLink() ? fs.existsSync(fs.realpathSync(ROOTRUN)) : true
  } catch {}
  if (present) {
    try {
      fs.chmodSync(src, 0o755)
    } catch {}
    return null
  }
  try {
    fs.rmSync(ROOTRUN, { force: true })
    fs.chmodSync(src, 0o755)
    fs.symlinkSync(src, ROOTRUN)
    return 'rootrun 软链'
  } catch {
    return null
  }
}

function ensureProfile() {
  const text = readText(PROFILE)
  if (text == null) return null
  if (text.includes('runnerCommand')) return null
  if (text.includes('id: sandbox')) return null
  writeAtomic(PROFILE, text.replace(/\s+$/, '') + '\n' + PROFILE_BLOCK + '\n')
  return 'profile runnerCommand（重启引擎生效）'
}

function repairAll(which) {
  const out = []
  const steps = [
    ['agents', ensureAgents],
    ['client', ensureClient],
    ['shim', ensureShim],
    ['rootrun', ensureRootrun],
    ['profile', ensureProfile],
  ]
  for (const step of steps) {
    if (which && which.indexOf(step[0]) < 0) continue
    try {
      const got = step[1]()
      if (got) out.push(got)
    } catch (err) {
      out.push(step[0] + ' 修复失败')
    }
  }
  return out
}

async function health() {
  try {
    const res = await fetch(BASE + '/health', { signal: AbortSignal.timeout(HEALTH_TIMEOUT) })
    const body = await res.json().catch(() => null)
    if (!res.ok || !body || body.ok !== true) {
      cache.online = false
      cache.note = '健康检查返回 ' + res.status
      cache.at = Date.now()
      return cache
    }
    cache.online = true
    cache.pid = Number(body.pid) || 0
    cache.uptimeMs = Number(body.uptimeMs) || 0
    cache.requests = Number(body.requests) || 0
    cache.note = '正常'
    cache.at = Date.now()
  } catch (err) {
    cache.online = false
    cache.note = err && err.name === 'TimeoutError' ? '连接超时' : '连不上 127.0.0.1:' + PORT
    cache.at = Date.now()
  }
  if (checkState.items.length) runCheck()
  return cache
}

async function execRoot(cmd, timeoutMs) {
  const token = readToken()
  if (!token) return { ok: false, error: '读不到令牌：先在 SukiSU 模块页点 Root Agent 的「执行」' }
  try {
    const res = await fetch(BASE + '/exec', {
      method: 'POST',
      headers: { authorization: 'Bearer ' + token, 'content-type': 'application/json' },
      body: JSON.stringify({ cmd, timeoutMs }),
      signal: AbortSignal.timeout(Math.max(timeoutMs, 5000) + 3000),
    })
    const body = await res.json().catch(() => null)
    if (!body) return { ok: false, error: '返回不是 JSON（HTTP ' + res.status + '）' }
    if (res.status === 401) return { ok: false, error: '令牌无效：重新点模块的「执行」取新令牌' }
    if (res.status === 429) return { ok: false, error: '守护进程正忙，稍等再试' }
    return body
  } catch (err) {
    return { ok: false, error: '连不上 Root Agent（模块装了吗？重启过手机吗？）' }
  }
}

function usageText() {
  const token = readToken()
  return [
    'Root Agent 用法（令牌只在本机可用）：',
    'curl -s -X POST http://127.0.0.1:' + PORT + '/exec \\',
    '  -H "Authorization: Bearer ' + (token || '<令牌>') + '" \\',
    '  -H "content-type: application/json" \\',
    '  -d \'{"cmd":"id -u"}\'',
  ].join('\n')
}

function duration(ms) {
  if (!ms) return '—'
  const s = Math.floor(ms / 1000)
  if (s < 60) return s + ' 秒'
  if (s < 3600) return Math.floor(s / 60) + ' 分钟'
  return Math.floor(s / 3600) + ' 小时'
}

function checkSummary() {
  if (!checkState.items.length) return '还没检查'
  const bad = checkState.items.filter((it) => it.state !== 'ok')
  if (!bad.length) return '全部正常'
  return bad.length + ' 项待处理：' + bad.map((it) => it.label).join('、').slice(0, 40)
}

function bootNote() {
  const boot = checkState.boot
  if (!boot) return '引擎启动后会自动检查一次'
  if (boot.error) return '启动自检出错：' + boot.error
  if (boot.changed && boot.changed.length) return '启动时已自动修复：' + boot.changed.join('、')
  return '启动自检：无需修复'
}

function checkListItems() {
  return checkState.items.map((it) => ({
    title: it.label,
    subtitle: it.detail,
    trailing: it.state === 'ok' ? '正常' : '待处理',
    tone: it.state === 'ok' ? 'good' : it.state === 'warn' ? 'warn' : 'bad',
  }))
}

function snapshot() {
  const token = readToken()
  return {
    state: cache.online ? '在线' : '离线',
    addr: '127.0.0.1:' + PORT,
    pid: cache.pid ? String(cache.pid) : '—',
    token: mask(token),
    requests: String(cache.requests),
    uptime: duration(cache.uptimeMs),
    note: cache.note,
    check: checkSummary(),
    boot: bootNote(),
    selfTest: selfTest.text,
    hint: cache.online
      ? '点「自检」用 root 跑一条命令试试；令牌文件在 /root/.dsh/rootagent-token（属主 App，权限 600）。'
      : '守护进程没在跑：确认 SukiSU 里 Root Agent 模块已启用并重启过手机；第一次用要先点模块卡片的「执行」拿令牌。',
  }
}

function bootCheck() {
  try {
    const changed = repairAll(['agents', 'client', 'shim', 'rootrun'])
    runCheck()
    checkState.boot = { at: Date.now(), changed }
    if (changed.length) {
      console.log('[local-root-agent] 启动自检修复：' + changed.join('、'))
    }
  } catch (err) {
    checkState.boot = { at: Date.now(), changed: [], error: String((err && err.message) || err) }
  }
}

runCheck()

export function apply(ctx) {
  if (process.env.DSH_ROOT_AGENT_NO_AUTO !== '1') {
    const timer = setTimeout(bootCheck, 1500)
    if (timer.unref) timer.unref()
  }

  ctx.inject(['dshGo'], (ctx) => {
    health()
    const poll = setInterval(health, 5000)
    if (poll.unref) poll.unref()
    ctx.effect(() => () => clearInterval(poll))

    ctx.dshGo.strip('status', (scope, emit) => {
      const tick = () => {
        const bad = checkState.items.filter((it) => it.state !== 'ok' && !(it.id === 'daemon' && !cache.at)).length
        emit({
          text: bad ? '沙箱 ' + bad + ' 项待修' : cache.online ? 'Root 在线 :' + PORT : 'Root 未连接',
          icon: 'terminal',
        })
      }
      tick()
      const timer = setInterval(tick, 4000)
      if (timer.unref) timer.unref()
      return () => clearInterval(timer)
    })

    ctx.dshGo.panel('root', {
      view: () => ({
        type: 'stack',
        title: 'Root 代理',
        children: [
          { type: 'text', text: 'dsh-go 在 proot 里只有假 root；这个面板调用 Android 侧 Root Agent 守护进程执行真 root 命令。', tone: 'muted' },
          {
            type: 'keyValue',
            items: [
              ['状态', { $: 'data.state' }],
              ['地址', { $: 'data.addr' }],
              ['令牌', { $: 'data.token' }],
              ['进程', { $: 'data.pid' }],
              ['运行', { $: 'data.uptime' }],
              ['请求', { $: 'data.requests' }],
              ['自检', { $: 'data.check' }],
            ],
          },
          {
            type: 'actions',
            items: [
              { label: '自检', action: 'selftest', style: 'primary' },
              { label: '刷新', action: 'refresh' },
              { label: '复制令牌', action: 'copyToken' },
              { label: '复制用法', action: 'copyUsage' },
            ],
          },
          { type: 'text', text: '升级自检', style: { size: 'title', weight: 'bold' } },
          { type: 'text', text: { $: 'data.boot' }, tone: 'muted' },
          { type: 'list', items: checkListItems() },
          {
            type: 'actions',
            items: [
              { label: '重新检查', action: 'recheck' },
              { label: '修复', action: 'repair' },
              { label: '补配置', action: 'fixProfile' },
            ],
          },
          { type: 'callout', tone: 'warn', text: '令牌等同 root 权限：不要截图外发，也不要把文件发给别人。' },
          { type: 'text', text: '最近自检', style: { size: 'title', weight: 'bold' } },
          { type: 'text', text: { $: 'data.selfTest' } },
          { type: 'callout', tone: 'info', text: { $: 'data.hint' } },
        ],
      }),
      data: (scope, emit) => {
        runCheck()
        const tick = () => emit(snapshot())
        tick()
        const timer = setInterval(tick, 4000)
        if (timer.unref) timer.unref()
        return () => clearInterval(timer)
      },
      actions: {
        refresh: () => {
          health()
          return { toast: '已刷新', refresh: true }
        },
        recheck: () => {
          runCheck()
          return { toast: checkSummary().slice(0, 30), refresh: true }
        },
        repair: () => {
          const fixed = repairAll(['agents', 'client', 'shim', 'rootrun', 'profile'])
          runCheck()
          return {
            toast: fixed.length ? '已修复：' + fixed.join('、').slice(0, 28) : '没有需要修的项',
            refresh: true,
          }
        },
        fixProfile: () => {
          const fixed = repairAll(['profile'])
          runCheck()
          return {
            toast: fixed.length ? '已补配置，重启引擎生效' : '配置已存在，无需补',
            refresh: true,
          }
        },
        selftest: () => ({
          pending: { label: '正在用 root 执行…', timeoutMs: 20000 },
          result: async () => {
            const res = await execRoot('id -u; getprop ro.product.model; getenforce 2>/dev/null || true', 15000)
            if (res && res.ok && res.code === 0) {
              const text = String(res.stdout || '').trim().replace(/\n+/g, ' · ').slice(0, 120)
              selfTest.at = Date.now()
              selfTest.ok = true
              selfTest.text = '通过：' + text
              return { toast: '自检通过：' + text.slice(0, 30) }
            }
            selfTest.at = Date.now()
            selfTest.ok = false
            selfTest.text = '失败：' + ((res && (res.error || ('退出码 ' + res.code))) || '未知错误')
            return { toast: '自检失败，看面板里的说明' }
          },
        }),
        copyToken: () => {
          const token = readToken()
          return token ? { copy: token, toast: '已复制令牌' } : { toast: '读不到令牌，先点模块的「执行」' }
        },
        copyUsage: () => ({ copy: usageText(), toast: '已复制用法' }),
      },
    })
  })
}

