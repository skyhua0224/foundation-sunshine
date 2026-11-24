import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'

// foundation-ui draws no picture of 薇薇 and has no wallpaper of hers until a product registers them: every page of the Web UI starts in init.js
const read = (f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')

test('init.js registers 薇薇 and her wallpapers before any page mounts', () => {
  const src = read('init.js')
  assert.match(src, /import \{ registerViviAssets \} from 'foundation-ui\/brand-assets'/)
  assert.match(src, /^registerViviAssets\(\)$/m)
  assert.ok(src.indexOf('registerViviAssets()') < src.indexOf('export async function initApp'))
})

test('the components that show the product name get it from the Web UI', () => {
  assert.match(read('config/brand.js'), /PRODUCT_NAME = 'Foundation Sunshine'/)
  assert.match(read('views/Login.vue'), /<MLoginPage :name="PRODUCT_NAME"/)
  assert.match(read('views/WebShell.vue'), /<MBrand class="web-caption__brand" :name="PRODUCT_NAME"/)
  assert.match(read('views/WebShell.vue'), /<MScene [^\n]*:brand-name="PRODUCT_NAME"/)
  for (const v of ['Doctor', 'Troubleshooting']) assert.match(read(`views/${v}.vue`), /:product="PRODUCT_NAME"/)
})
