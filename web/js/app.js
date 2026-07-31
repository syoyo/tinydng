import TinyDngModule from "./dist/tinydng.js";
import { createTinyDng } from "./tinydng-api.js";
import { RawDeveloper } from "./raw-develop.js";

const fileInput = document.querySelector("#file");
const imageSelect = document.querySelector("#image");
const status = document.querySelector("#status");
const canvas = document.querySelector("#canvas");
const fallbackCanvas = document.querySelector("#fallbackCanvas");
const metadata = document.querySelector("#metadata");
const exposure = document.querySelector("#exposure");
const shadow = document.querySelector("#shadow");
const highlight = document.querySelector("#highlight");
const colorCorrection = document.querySelector("#colorCorrection");
const exposureValue = document.querySelector("#exposureValue");
const shadowValue = document.querySelector("#shadowValue");
const highlightValue = document.querySelector("#highlightValue");

let decoder;
let documentHandle;
let infos = [];
let currentDecoded;
let renderGeneration = 0;
let gpuDeveloper;

function setStatus(message) {
  status.textContent = message;
}

function controls() {
  const values = {
    exposure: Number(exposure.value),
    shadow: Number(shadow.value),
    highlight: Number(highlight.value),
    colorCorrection: colorCorrection.checked,
  };
  exposureValue.value = `${values.exposure.toFixed(1)} EV`;
  shadowValue.value = `${Math.round(values.shadow * 100)}%`;
  highlightValue.value = `${Math.round(values.highlight * 100)}%`;
  return values;
}

function formatCompression(value) {
  return {
    1: "None",
    5: "LZW",
    6: "Old JPEG",
    7: "Lossless JPEG",
    8: "ZIP",
    32773: "PackBits",
  }[value] || `Compression ${value}`;
}

function formatSampleFormat(value) {
  return { 1: "Unsigned integer", 2: "Signed integer", 3: "IEEE float" }[value] || `Format ${value}`;
}

function setMetadata(info) {
  const rows = [
    ["Dimensions", `${info.width} × ${info.height}`],
    ["Channels", info.samplesPerPixel],
    ["Stored bits", info.bitsPerSample],
    ["Decoded bits", info.bitsPerSampleDecoded],
    ["Sample format", formatSampleFormat(info.sampleFormat)],
    ["Compression", formatCompression(info.compression)],
    ["Orientation", info.orientation || "Not specified"],
    ["Segments", info.segmentCount],
    ["Make", info.make || "—"],
    ["Model", info.model || "—"],
    ["CFA", info.cfa ? "Present (GPU demosaic)" : "No"],
  ];
  if (info.blackLevelPresent) rows.push(["Black level", info.blackLevel]);
  if (info.whiteLevelPresent) rows.push(["White level", info.whiteLevel]);
  metadata.replaceChildren(...rows.flatMap(([label, value]) => {
    const dt = document.createElement("dt");
    const dd = document.createElement("dd");
    dt.textContent = label;
    dd.textContent = String(value);
    return [dt, dd];
  }));
}

function normalizedSample(bytes, offset, info, channel) {
  if (info.sampleFormat === 3) {
    const value = new DataView(bytes.buffer, bytes.byteOffset + offset, 4).getFloat32(0, true);
    return Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0;
  }
  if (info.bitsPerSampleDecoded > 8) {
    const view = new DataView(bytes.buffer, bytes.byteOffset + offset, 2);
    const value = info.sampleFormat === 2 ? view.getInt16(0, true) : view.getUint16(0, true);
    if (info.sampleFormat === 2) return Math.max(0, Math.min(1, (value + 32768) / 65535));
    const white = info.whiteLevelPresent && info.whiteLevel > 0
      ? info.whiteLevel
      : (2 ** info.bitsPerSample) - 1;
    return Math.max(0, Math.min(1, (value - (info.blackLevelPresent ? info.blackLevel : 0)) / Math.max(1, white - (info.blackLevelPresent ? info.blackLevel : 0))));
  }
  return info.sampleFormat === 2 ? (bytes[offset] + 128) / 255 : bytes[offset] / 255;
}

function drawPreview(decoded) {
  const { info, data } = decoded;
  const maxEdge = 2048;
  const scale = Math.min(1, maxEdge / Math.max(info.width, info.height));
  const width = Math.max(1, Math.round(info.width * scale));
  const height = Math.max(1, Math.round(info.height * scale));
  const output = new ImageData(width, height);
  const bytesPerSample = info.pixelStride / info.samplesPerPixel;
  const channels = info.samplesPerPixel;
  for (let y = 0; y < height; y += 1) {
    const sourceY = Math.min(info.height - 1, Math.floor(y / scale));
    for (let x = 0; x < width; x += 1) {
      const sourceX = Math.min(info.width - 1, Math.floor(x / scale));
      const source = (sourceY * info.width + sourceX) * info.pixelStride;
      const red = normalizedSample(data, source, info, 0);
      const green = normalizedSample(data, source + (channels > 1 ? bytesPerSample : 0), info, 1);
      const blue = normalizedSample(data, source + (channels > 2 ? bytesPerSample * 2 : 0), info, 2);
      const gray = red;
      const pixel = (y * width + x) * 4;
      output.data[pixel] = Math.round((channels > 1 ? red : gray) * 255);
      output.data[pixel + 1] = Math.round((channels > 1 ? green : gray) * 255);
      output.data[pixel + 2] = Math.round((channels > 2 ? blue : gray) * 255);
      output.data[pixel + 3] = 255;
    }
  }
  fallbackCanvas.width = width;
  fallbackCanvas.height = height;
  fallbackCanvas.hidden = false;
  canvas.hidden = true;
  fallbackCanvas.getContext("2d").putImageData(output, 0, 0);
}

function selectLargest() {
  let largest = 0;
  for (let i = 1; i < infos.length; i += 1) {
    if (infos[i].width * infos[i].height > infos[largest].width * infos[largest].height) largest = i;
  }
  imageSelect.value = String(largest);
  return largest;
}

async function developCurrent() {
  if (!currentDecoded) return;
  const generation = ++renderGeneration;
  const info = currentDecoded.info;
  const values = controls();
  setStatus(`Developing ${info.width} × ${info.height}…`);
  try {
    if (gpuDeveloper) {
      canvas.hidden = false;
      fallbackCanvas.hidden = true;
      await gpuDeveloper.render(currentDecoded, values, (progress) => {
        if (generation === renderGeneration) {
          setStatus(`Developing ${info.width} × ${info.height} (${Math.round(progress * 100)}%)…`);
        }
        return generation === renderGeneration;
      });
    } else {
      drawPreview(currentDecoded);
    }
    if (generation === renderGeneration) {
      setStatus(`Developed ${info.width} × ${info.height}`);
    }
  } catch (error) {
    if (generation !== renderGeneration) return;
    drawPreview(currentDecoded);
    setStatus(`WebGL2 development failed; raw fallback: ${error.message}`);
  }
}

async function render(index) {
  if (!documentHandle) return;
  const info = infos[index];
  setMetadata(info);
  setStatus(`Decoding image ${index + 1}/${infos.length}…`);
  currentDecoded = documentHandle.decode(index);
  await developCurrent();
}

async function openFile(file) {
  if (documentHandle) documentHandle.close();
  documentHandle = undefined;
  currentDecoded = undefined;
  canvas.width = 0;
  canvas.height = 0;
  fallbackCanvas.width = 0;
  fallbackCanvas.height = 0;
  canvas.hidden = true;
  fallbackCanvas.hidden = true;
  metadata.replaceChildren();
  imageSelect.replaceChildren();
  imageSelect.disabled = true;
  setStatus(`Opening ${file.name}…`);
  try {
    documentHandle = decoder.open(new Uint8Array(await file.arrayBuffer()));
    infos = documentHandle.imageInfos();
    for (let i = 0; i < infos.length; i += 1) {
      const option = document.createElement("option");
      option.value = String(i);
      option.textContent = `IFD ${i}: ${infos[i].width} × ${infos[i].height}`;
      imageSelect.append(option);
    }
    imageSelect.disabled = false;
    await render(selectLargest());
  } catch (error) {
    if (documentHandle) documentHandle.close();
    documentHandle = undefined;
    setStatus(error.message);
  }
}

fileInput.addEventListener("change", () => {
  if (fileInput.files[0]) openFile(fileInput.files[0]);
});
imageSelect.addEventListener("change", () => render(Number(imageSelect.value)));
[exposure, shadow, highlight, colorCorrection].forEach((control) => {
  control.addEventListener("input", () => developCurrent());
});
controls();

try {
  try {
    gpuDeveloper = new RawDeveloper(canvas);
  } catch (error) {
    gpuDeveloper = undefined;
    setStatus(`WebGL2 unavailable; Canvas2D fallback (${error.message})`);
  }
  decoder = await createTinyDng(TinyDngModule);
  if (gpuDeveloper) setStatus("Ready (WebGL2 tiled raw developer)");
} catch (error) {
  setStatus(`Decoder initialization failed: ${error.message}`);
}
