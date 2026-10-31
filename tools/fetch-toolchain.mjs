#!/usr/bin/env node
// 下载 winlibs 免安装 MinGW-w64 工具链（无管理员权限可用）。
// 用法: node tools/fetch-toolchain.mjs [目标目录，默认 .toolchain]
import { createWriteStream } from 'node:fs';
import { mkdir, readFile, writeFile, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { Readable, Transform } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import path from 'node:path';

const REPO = 'brechtsanders/winlibs_mingw';
// 固定到已验证的版本：重建时不应漂移到未经测试的新 GCC。
// 升级工具链时改这里，并在 docs/DESIGN.md 记录验证结果。
const PINNED_TAG = '16.2.0posix-23.1.3-14.0.0-ucrt-r3';
const ASSET_RE = /^winlibs-x86_64-posix-seh-gcc-[\d.]+-mingw-w64ucrt-[\d.]+-r\d+\.zip$/;
const HEADERS = { 'user-agent': 'etools-bootstrap/1.0' };
const destDir = path.resolve(process.argv[2] ?? '.toolchain');
const downloadDir = path.join(destDir, 'download');
const zipPath = path.join(downloadDir, 'winlibs.zip');

function log(msg) {
  process.stdout.write(`[fetch-toolchain] ${msg}\n`);
}

async function getJson(url) {
  const res = await fetch(url, { headers: { ...HEADERS, accept: 'application/vnd.github+json' } });
  if (!res.ok) throw new Error(`GET ${url} -> HTTP ${res.status}`);
  return res.json();
}

async function getAssetText(asset) {
  const url = `https://api.github.com/repos/${REPO}/releases/assets/${asset.id}`;
  const res = await fetch(url, { headers: { ...HEADERS, accept: 'application/octet-stream' } });
  if (!res.ok) throw new Error(`GET ${url} -> HTTP ${res.status}`);
  return res.text();
}

async function downloadAsset(asset, outFile) {
  const url = `https://api.github.com/repos/${REPO}/releases/assets/${asset.id}`;
  const res = await fetch(url, { headers: { ...HEADERS, accept: 'application/octet-stream' } });
  if (!res.ok || !res.body) throw new Error(`download ${asset.name} -> HTTP ${res.status}`);

  const total = Number(res.headers.get('content-length') ?? asset.size ?? 0);
  const hash = createHash('sha256');
  let received = 0;
  let lastPct = -5;
  const tap = new Transform({
    transform(chunk, _enc, cb) {
      received += chunk.length;
      hash.update(chunk);
      const pct = total ? Math.floor((received * 100) / total) : 0;
      if (pct >= lastPct + 5) {
        lastPct = pct;
        log(`  ${String(pct).padStart(3)}%  ${(received / 1048576).toFixed(0)}MB / ${(total / 1048576).toFixed(0)}MB`);
      }
      cb(null, chunk);
    },
  });
  await pipeline(Readable.fromWeb(res.body), tap, createWriteStream(outFile));
  return { bytes: received, sha256: hash.digest('hex') };
}

async function main() {
  await mkdir(downloadDir, { recursive: true });

  log(`查询 release ${PINNED_TAG} ...`);
  const release = await getJson(`https://api.github.com/repos/${REPO}/releases/tags/${PINNED_TAG}`);
  if (release.tag_name !== PINNED_TAG) {
    throw new Error(`返回的 tag 不匹配: 期望 ${PINNED_TAG}, 实际 ${release.tag_name}`);
  }
  const asset = (release.assets ?? []).find((a) => ASSET_RE.test(a.name));
  if (!asset) throw new Error(`release ${release.tag_name} 中没有匹配 ${ASSET_RE} 的资源`);
  const shaAsset = (release.assets ?? []).find((a) => a.name === `${asset.name}.sha256`);
  log(`目标: ${asset.name} (${(asset.size / 1048576).toFixed(0)}MB, tag ${release.tag_name})`);

  let result;
  try {
    const st = await stat(zipPath);
    if (st.size === asset.size) {
      log('已存在同尺寸压缩包，复用并校验 ...');
      const hash = createHash('sha256');
      hash.update(await readFile(zipPath));
      result = { bytes: st.size, sha256: hash.digest('hex') };
    }
  } catch {
    /* 不存在则下载 */
  }

  if (!result) {
    log('开始下载 ...');
    result = await downloadAsset(asset, zipPath);
  }

  // 优先使用 GitHub API 的 digest 字段校验 zip 本体；退而用发布页的 .sha256 文件。
  let expected = null;
  if (typeof asset.digest === 'string' && asset.digest.startsWith('sha256:')) {
    expected = asset.digest.slice('sha256:'.length).toLowerCase();
  } else if (shaAsset) {
    const text = await getAssetText(shaAsset);
    expected = text.trim().split(/\s+/)[0]?.toLowerCase() ?? null;
  }
  if (expected) {
    if (expected !== result.sha256) {
      throw new Error(`SHA256 不匹配: 期望 ${expected}, 实际 ${result.sha256}`);
    }
    log(`SHA256 校验通过: ${result.sha256}`);
  } else {
    log(`未取得官方校验值，跳过校验 (sha256=${result.sha256})`);
  }

  const manifest = {
    repo: REPO,
    tag: release.tag_name,
    asset: asset.name,
    bytes: result.bytes,
    sha256: result.sha256,
    source: asset.digest ? 'api-digest' : 'sha256-asset',
    downloadedAt: new Date().toISOString(),
  };
  await writeFile(path.join(destDir, 'manifest.json'), JSON.stringify(manifest, null, 2), 'utf8');
  log(`完成: ${zipPath}`);
}

main().catch((err) => {
  process.stderr.write(`[fetch-toolchain] 失败: ${err.stack ?? err.message}\n`);
  process.exit(1);
});
