#!/usr/bin/env node
// 下载并校验 Everything SDK（voidtools 官方包），只解出 Lume 需要的三个源码文件。
// 用法:
//   node tools/fetch-everything-sdk.mjs                 # 下载（有本地缓存则复用并校验）
//   node tools/fetch-everything-sdk.mjs --local=<zip>   # 直接用已有的 Everything-SDK.zip
//   node tools/fetch-everything-sdk.mjs --out=<dir>     # 改输出目录（默认 third_party/everything）
//
// 说明：SDK 里同时提供 dll/Everything64.dll 与 lib/Everything64.lib，本项目改用
// src/Everything.c 的源码编进 exe（见 third_party/everything/README.md），所以只解三个文件。
import { readFile, writeFile, mkdir, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { inflateRawSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const SDK_URL = 'https://www.voidtools.com/Everything-SDK.zip';
// 固定到已验证的整包：重建时不应漂移到未经测试的新 SDK。
// 升级 SDK 时改这里与 FILES 的大小/哈希，并更新 third_party/everything/README.md 的表格。
const SDK_SHA256 = 'f5716d9513cce6b462b5170a0a2e7e081e191d9bcac6774f5decc061497df443';
const SDK_BYTES = 238231;
const FILES = [
  { from: 'include/Everything.h', to: 'include/Everything.h', bytes: 15300 },
  { from: 'ipc/everything_ipc.h', to: 'ipc/Everything_IPC.h', bytes: 50671 },
  { from: 'src/Everything.c', to: 'src/Everything.c', bytes: 71123 },
];

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let outDir = path.join(repoRoot, 'third_party', 'everything');
let localZip = '';
for (const arg of process.argv.slice(2)) {
  if (arg.startsWith('--local=')) localZip = path.resolve(arg.slice('--local='.length));
  else if (arg.startsWith('--out=')) outDir = path.resolve(arg.slice('--out='.length));
  else if (arg === '--help' || arg === '-h') {
    process.stdout.write('usage: node tools/fetch-everything-sdk.mjs [--local=<Everything-SDK.zip>] [--out=<dir>]\n');
    process.exit(0);
  } else {
    process.stderr.write(`未知参数: ${arg}\n`);
    process.exit(2);
  }
}

function log(msg) {
  process.stdout.write(`[fetch-everything-sdk] ${msg}\n`);
}

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest('hex');
}

async function exists(file) {
  try {
    const st = await stat(file);
    return st.isFile();
  } catch {
    return false;
  }
}

// 读 ZIP 的中央目录（不依赖第三方库；本包无 Zip64/加密，够用）。
function readZip(buffer) {
  let eocd = -1;
  const lowest = Math.max(0, buffer.length - 22 - 0xffff);
  for (let i = buffer.length - 22; i >= lowest; --i) {
    if (buffer.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('不是有效的 ZIP：找不到 EOCD');
  const count = buffer.readUInt16LE(eocd + 10);
  const centralOffset = buffer.readUInt32LE(eocd + 16);
  const entries = new Map();
  let p = centralOffset;
  for (let i = 0; i < count; ++i) {
    if (buffer.readUInt32LE(p) !== 0x02014b50) throw new Error(`中央目录损坏（第 ${i} 项）`);
    const method = buffer.readUInt16LE(p + 10);
    const compressedSize = buffer.readUInt32LE(p + 20);
    const nameLength = buffer.readUInt16LE(p + 28);
    const extraLength = buffer.readUInt16LE(p + 30);
    const commentLength = buffer.readUInt16LE(p + 32);
    const localOffset = buffer.readUInt32LE(p + 42);
    const name = buffer.subarray(p + 46, p + 46 + nameLength).toString('utf8');
    entries.set(name, { method, compressedSize, localOffset });
    p += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

function extractEntry(buffer, entry) {
  const p = entry.localOffset;
  if (buffer.readUInt32LE(p) !== 0x04034b50) throw new Error('本地文件头损坏');
  const nameLength = buffer.readUInt16LE(p + 26);
  const extraLength = buffer.readUInt16LE(p + 28);
  const start = p + 30 + nameLength + extraLength;
  const raw = buffer.subarray(start, start + entry.compressedSize);
  if (entry.method === 0) return Buffer.from(raw);
  if (entry.method === 8) return inflateRawSync(raw);
  throw new Error(`不支持的压缩方式 ${entry.method}`);
}

async function loadArchive() {
  const cache = path.join(outDir, 'download', 'Everything-SDK.zip');
  const candidates = [];
  if (localZip) candidates.push(localZip);
  candidates.push(cache, path.join(repoRoot, 'Everything-SDK.zip'));

  for (const candidate of candidates) {
    if (!(await exists(candidate))) continue;
    const buffer = await readFile(candidate);
    const digest = sha256(buffer);
    if (digest !== SDK_SHA256) {
      throw new Error(
        `${candidate} 的 SHA256 是 ${digest}，与锁定的 ${SDK_SHA256} 不一致。\n` +
          '若确实要升级 SDK，请同步更新本脚本的 SDK_SHA256/SDK_BYTES 与 third_party/everything/README.md。',
      );
    }
    log(`复用已有压缩包: ${candidate}（${buffer.length} bytes，SHA256 校验通过）`);
    return buffer;
  }

  log(`本地没有可用压缩包，开始下载 ${SDK_URL} ...`);
  const res = await fetch(SDK_URL);
  if (!res.ok) throw new Error(`GET ${SDK_URL} -> HTTP ${res.status}`);
  const buffer = Buffer.from(await res.arrayBuffer());
  const digest = sha256(buffer);
  if (digest !== SDK_SHA256) {
    throw new Error(`下载包的 SHA256 是 ${digest}，与锁定的 ${SDK_SHA256} 不一致（可能被替换或已更新版本）`);
  }
  await mkdir(path.dirname(cache), { recursive: true });
  await writeFile(cache, buffer);
  log(`已下载并校验: ${cache}（${buffer.length} bytes）`);
  return buffer;
}

async function main() {
  const buffer = await loadArchive();
  if (buffer.length !== SDK_BYTES) {
    log(`注意: 包大小 ${buffer.length} 与记录的 ${SDK_BYTES} 不同（哈希一致，可忽略）`);
  }
  const entries = readZip(buffer);

  for (const file of FILES) {
    const entry = entries.get(file.from);
    if (!entry) throw new Error(`压缩包中缺少 ${file.from}`);
    const data = extractEntry(buffer, entry);
    if (data.length !== file.bytes) {
      throw new Error(`${file.from} 解出 ${data.length} bytes，期望 ${file.bytes}（SDK 版本不一致？）`);
    }
    const target = path.join(outDir, file.to);
    await mkdir(path.dirname(target), { recursive: true });
    await writeFile(target, data);
    log(`  ${file.from} -> ${path.relative(repoRoot, target)}  ${data.length} bytes  sha256=${sha256(data)}`);
  }
  log('完成（文件逐字节未修改，可直接 git diff 验证）');
}

main().catch((err) => {
  process.stderr.write(`[fetch-everything-sdk] 失败: ${err.stack ?? err.message}\n`);
  process.exit(1);
});
