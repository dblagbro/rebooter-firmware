import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const root = new URL("../", import.meta.url);
const appJs = await readFile(new URL("../data/app.js", import.meta.url), "utf8");
const indexHtml = await readFile(new URL("../data/index.html", import.meta.url), "utf8");

const checks = [
  {
    name: "index includes auth form",
    test: () => {
      assert.match(indexHtml, /id="auth-form"/);
      assert.match(indexHtml, /id="auth-password"/);
      assert.match(indexHtml, /id="auth-clear"/);
    },
  },
  {
    name: "protected controls are marked",
    test: () => {
      assert.match(indexHtml, /data-protected="true"/);
      assert.match(appJs, /updateProtectedControls/);
    },
  },
  {
    name: "auth token persists only for current tab session",
    test: () => {
      assert.match(appJs, /sessionStorage\.getItem\('rebooterAuth'\)/);
      assert.match(appJs, /sessionStorage\.setItem\('rebooterAuth', state\.authToken\)/);
      assert.match(appJs, /sessionStorage\.removeItem\('rebooterAuth'\)/);
    },
  },
  {
    name: "fetch requests attach X-Rebooter-Auth",
    test: () => {
      assert.match(appJs, /headers\.set\('X-Rebooter-Auth', state\.authToken\)/);
    },
  },
  {
    name: "OTA upload attaches X-Rebooter-Auth",
    test: () => {
      assert.match(appJs, /xhr\.setRequestHeader\('X-Rebooter-Auth', state\.authToken\)/);
    },
  },
];

const results = [];
for (const check of checks) {
  try {
    check.test();
    results.push({ name: check.name, passed: true });
  } catch (error) {
    results.push({ name: check.name, passed: false, error: error.message });
  }
}

const failed = results.filter((item) => !item.passed);
console.log(JSON.stringify({
  date: new Date().toISOString(),
  checks: results,
  summary: {
    total: results.length,
    passed: results.length - failed.length,
    failed: failed.length,
  },
}, null, 2));

if (failed.length) {
  process.exit(1);
}
