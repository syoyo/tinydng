import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import process from "node:process";

import TinyDngModule from "./dist/tinydng.js";
import { createTinyDng } from "./tinydng-api.js";

const root = process.argv[2] || path.resolve("../..");
const decoder = await createTinyDng(TinyDngModule);

const dng = decoder.open(new Uint8Array(await fs.readFile(path.join(root, "colorchart.dng"))));
assert.ok(dng.valid);
assert.ok(dng.imageCount >= 1);
const dngInfo = dng.imageInfo(0);
assert.ok(dngInfo.width > 0 && dngInfo.height > 0);
assert.ok(dngInfo.cfa && dngInfo.cfaPatternSize >= 4);
assert.ok(dngInfo.colorMatrixPresent && dngInfo.asShotNeutralPresent);
const dngPixels = dng.decode(0);
assert.equal(dngPixels.data.byteLength, dngInfo.imageSize);
assert.ok(dngPixels.data.some((value) => value !== 0));
dng.close();

const subifd = decoder.open(new Uint8Array(await fs.readFile(path.join(root, "pixel3.dng"))));
assert.ok(subifd.imageInfos().some((info) => info.width >= 3000));
subifd.close();

const tiff = decoder.open(new Uint8Array(await fs.readFile(path.join(root, "tests/v3_test/data/baseline_rgb8.tiff"))));
const tiffInfo = tiff.imageInfo(0);
assert.equal(tiffInfo.samplesPerPixel, 3);
const tiffPixels = tiff.decode(0);
assert.equal(tiffPixels.data.byteLength, tiffInfo.imageSize);
tiff.close();

let rejected = false;
try {
  decoder.open(new Uint8Array([0, 1, 2, 3, 4]));
} catch (error) {
  rejected = true;
  assert.ok(error.message.length > 0);
}
assert.ok(rejected);
console.log("WASM web smoke tests passed");
