import assert from 'node:assert/strict'
import { existsSync, readFileSync } from 'node:fs'
import test from 'node:test'

// the default entries that the Windows host ships: apps.json (English) and apps.zh.json (Chinese installer), their covers, and the names that follow the language
const windowsAssets = new URL('../../../../windows/assets/', import.meta.url)
const commonAssets = new URL('../../../assets/', import.meta.url)
const read = (name) => JSON.parse(readFileSync(new URL(name, windowsAssets), 'utf8'))
const pngSize = (url) => {
  const head = readFileSync(url).subarray(0, 24)
  assert.equal(head.subarray(1, 4).toString('latin1'), 'PNG')
  return [head.readUInt32BE(16), head.readUInt32BE(20)]
}
// A simple lossy WebP ("VP8 ") and an extended one ("VP8X", e.g. with alpha) both state the canvas size.
const webpSize = (url) => {
  const head = readFileSync(url).subarray(0, 32)
  assert.equal(head.subarray(0, 4).toString('latin1'), 'RIFF')
  assert.equal(head.subarray(8, 12).toString('latin1'), 'WEBP')
  const chunk = head.subarray(12, 16).toString('latin1')
  if (chunk === 'VP8X') return [head.readUIntLE(24, 3) + 1, head.readUIntLE(27, 3) + 1]
  assert.equal(chunk, 'VP8 ')
  assert.equal(head.subarray(23, 26).toString('hex'), '9d012a')
  return [head.readUInt16LE(26) & 0x3fff, head.readUInt16LE(28) & 0x3fff]
}
const locate = (name) => (existsSync(new URL(name, windowsAssets)) ? new URL(name, windowsAssets) : new URL(name, commonAssets))

const EXPECT = {
  desktop: { hero: 'desktop-hero.webp', en: 'Desktop', zh: '桌面' },
  'big-screen': { hero: 'bigscreen-hero.webp', en: 'Big Screen', zh: '大屏' },
  steam: { hero: 'steam-hero.webp', en: 'Steam Big Picture', zh: 'Steam Big Picture 模式' },
  xbox: { hero: 'box-hero.webp', en: 'Xbox Game', zh: 'Xbox 游戏' },
}

for (const [file, lang] of [['apps.json', 'en'], ['apps.zh.json', 'zh']]) {
  test(`${file}: every default entry is marked, ships a landscape cover, and is named in its language`, () => {
    const apps = read(file).apps
    assert.deepEqual(apps.map((app) => app['default-id']).sort(), Object.keys(EXPECT).sort())
    for (const app of apps) {
      const want = EXPECT[app['default-id']]
      assert.equal(app.hero, want.hero)
      assert.equal(app.name, want[lang])
      assert.ok(existsSync(locate(app.hero)), `${app.hero} exists`)
      const image = app['image-path']
      if (image !== 'desktop') assert.ok(existsSync(locate(image)), `${image} exists`)
    }
  })
}

test('the covers of the default entries have the aspect that they are named for', () => {
  for (const name of ['steam.png', 'box.png']) assert.deepEqual(pngSize(locate(name)), [600, 900], name)
  for (const name of ['steam-hero.webp', 'box-hero.webp', 'desktop-hero.webp', 'bigscreen-hero.webp']) assert.deepEqual(webpSize(locate(name)), [1920, 1080], name)
})

test('the art has a record: sources and terms in the assets README and the source index', () => {
  const readme = readFileSync(new URL('../../../../../assets/brand/default-apps/README.md', import.meta.url), 'utf8')
  assert.match(readme, /No AI-generated imagery/)
  assert.match(readme, /steam_brandAssets\.eps/)
  assert.match(readme, /FALLBACK/)
  const index = readFileSync(new URL('../../../../../docs/authority/source-index.tsv', import.meta.url), 'utf8')
  for (const key of ['valve-steam-brand-guidelines-20261110', 'valve-steam-brand-assets-eps-20261110', 'microsoft-trademark-brand-guidelines-20261110']) assert.ok(index.includes(key), key)
})
