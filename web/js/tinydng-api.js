const INFO_SIZE = 616;
const INFO = {
  width: 0,
  height: 4,
  imageSize: 8,
  pixelStride: 12,
  segmentCount: 16,
  samplesPerPixel: 20,
  bitsPerSample: 24,
  bitsPerSampleDecoded: 28,
  sampleFormat: 32,
  photometric: 36,
  compression: 40,
  orientation: 44,
  flags: 48,
  blackLevel: 52,
  whiteLevel: 56,
  make: 60,
  model: 188,
  software: 316,
  datetime: 444,
  colorMatrix: 476,
  asShotNeutral: 512,
  cfaPatternRows: 524,
  cfaPatternCols: 528,
  cfaPatternSize: 532,
  cfaPattern: 536,
  planeColor: 600,
};

const STATUS = {
  OK: 0,
  INVALID_ARG: 1,
  PARSE: 2,
  UNSUPPORTED: 3,
  IO: 4,
  OOM: 5,
  BOUNDS: 6,
  DECODE: 7,
  INTERNAL: 8,
};

function readCString(module, pointer, maxLength) {
  const bytes = module.HEAPU8;
  let end = pointer;
  const limit = pointer + maxLength;
  while (end < limit && bytes[end] !== 0) end += 1;
  return new TextDecoder().decode(bytes.subarray(pointer, end));
}

function statusError(status, message) {
  const error = new Error(message || `TinyDNG error ${status}`);
  error.status = status;
  return error;
}

function readInfo(module, pointer, infoSize) {
  const view = new DataView(module.HEAPU8.buffer, pointer, infoSize);
  const flags = view.getUint32(INFO.flags, true);
  return {
    width: view.getUint32(INFO.width, true),
    height: view.getUint32(INFO.height, true),
    imageSize: view.getUint32(INFO.imageSize, true),
    pixelStride: view.getUint32(INFO.pixelStride, true),
    segmentCount: view.getUint32(INFO.segmentCount, true),
    samplesPerPixel: view.getUint32(INFO.samplesPerPixel, true),
    bitsPerSample: view.getUint32(INFO.bitsPerSample, true),
    bitsPerSampleDecoded: view.getUint32(INFO.bitsPerSampleDecoded, true),
    sampleFormat: view.getUint32(INFO.sampleFormat, true),
    photometric: view.getUint32(INFO.photometric, true),
    compression: view.getUint32(INFO.compression, true),
    orientation: view.getUint32(INFO.orientation, true),
    cfa: Boolean(flags & 1),
    blackLevelPresent: Boolean(flags & 2),
    whiteLevelPresent: Boolean(flags & 4),
    blackLevel: view.getInt32(INFO.blackLevel, true),
    whiteLevel: view.getUint32(INFO.whiteLevel, true),
    colorMatrixPresent: Boolean(flags & 8),
    asShotNeutralPresent: Boolean(flags & 16),
    colorMatrix: Array.from({ length: 9 }, (_, i) =>
      view.getFloat32(INFO.colorMatrix + i * 4, true)),
    asShotNeutral: Array.from({ length: 3 }, (_, i) =>
      view.getFloat32(INFO.asShotNeutral + i * 4, true)),
    cfaPatternRows: view.getUint32(INFO.cfaPatternRows, true),
    cfaPatternCols: view.getUint32(INFO.cfaPatternCols, true),
    cfaPatternSize: view.getUint32(INFO.cfaPatternSize, true),
    cfaPattern: Array.from({ length: 16 }, (_, i) =>
      view.getUint32(INFO.cfaPattern + i * 4, true)),
    planeColor: Array.from({ length: 4 }, (_, i) =>
      view.getUint32(INFO.planeColor + i * 4, true)),
    make: readCString(module, pointer + INFO.make, 128),
    model: readCString(module, pointer + INFO.model, 128),
    software: readCString(module, pointer + INFO.software, 128),
    datetime: readCString(module, pointer + INFO.datetime, 32),
  };
}

export class TinyDngDocument {
  constructor(module, handle, inputPointer) {
    this.module = module;
    this.handle = handle;
    this.inputPointer = inputPointer;
    this.infoSize = module._tinydng_web_info_size();
    if (this.infoSize !== INFO_SIZE) {
      module._tinydng_web_close(handle);
      throw new Error(`Unexpected TinyDNG image info ABI size: ${this.infoSize}`);
    }
    this.infoPointer = module._malloc(this.infoSize);
    if (!this.infoPointer) {
      module._tinydng_web_close(handle);
      throw statusError(STATUS.OOM, "unable to allocate image info buffer");
    }
  }

  get valid() {
    return Boolean(this.handle && this.module._tinydng_web_is_valid(this.handle));
  }

  get error() {
    if (!this.handle) return "invalid document handle";
    const pointer = this.module._tinydng_web_error(this.handle);
    return this.module.UTF8ToString(pointer);
  }

  get imageCount() {
    return this.valid ? this.module._tinydng_web_image_count(this.handle) : 0;
  }

  imageInfo(index) {
    if (!this.valid) throw statusError(STATUS.INVALID_ARG, this.error);
    const status = this.module._tinydng_web_get_image_info(
      this.handle,
      index,
      this.infoPointer,
    );
    if (status !== STATUS.OK) throw statusError(status, this.error);
    return readInfo(this.module, this.infoPointer, this.infoSize);
  }

  imageInfos() {
    const images = [];
    for (let index = 0; index < this.imageCount; index += 1) {
      images.push(this.imageInfo(index));
    }
    return images;
  }

  decode(index) {
    const info = this.imageInfo(index);
    const outputPointer = this.module._malloc(info.imageSize);
    if (!outputPointer) throw statusError(STATUS.OOM, "unable to allocate decode buffer");
    try {
      const status = this.module._tinydng_web_decode(
        this.handle,
        index,
        outputPointer,
        info.imageSize,
      );
      if (status !== STATUS.OK) throw statusError(status, this.error);
      return { info, data: this.module.HEAPU8.slice(outputPointer, outputPointer + info.imageSize) };
    } finally {
      this.module._free(outputPointer);
    }
  }

  decodeRegion(index, x, y, width, height) {
    const info = this.imageInfo(index);
    const outputSize = width * height * info.pixelStride;
    if (!Number.isSafeInteger(outputSize) || outputSize <= 0 || outputSize > 0xffffffff) {
      throw statusError(STATUS.BOUNDS, "decoded region is too large");
    }
    const outputPointer = this.module._malloc(outputSize);
    if (!outputPointer) throw statusError(STATUS.OOM, "unable to allocate decode buffer");
    try {
      const status = this.module._tinydng_web_decode_region(
        this.handle,
        index,
        x,
        y,
        width,
        height,
        outputPointer,
        outputSize,
      );
      if (status !== STATUS.OK) throw statusError(status, this.error);
      return {
        info: { ...info, width, height, imageSize: outputSize },
        data: this.module.HEAPU8.slice(outputPointer, outputPointer + outputSize),
      };
    } finally {
      this.module._free(outputPointer);
    }
  }

  close() {
    if (this.infoPointer) this.module._free(this.infoPointer);
    this.infoPointer = 0;
    if (this.handle) this.module._tinydng_web_close(this.handle);
    this.handle = 0;
    this.inputPointer = 0;
  }
}

export async function createTinyDng(ModuleFactory) {
  const module = await ModuleFactory();
  return {
    module,
    open(bytes) {
      const input = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
      const inputPointer = module._malloc(input.byteLength);
      if (!inputPointer) throw statusError(STATUS.OOM, "unable to allocate input buffer");
      module.HEAPU8.set(input, inputPointer);
      const handle = module._tinydng_web_open(inputPointer, input.byteLength);
      if (!handle) {
        module._free(inputPointer);
        throw statusError(STATUS.OOM, "unable to allocate document handle");
      }
      const document = new TinyDngDocument(module, handle, inputPointer);
      if (!document.valid) {
        const message = document.error;
        const status = module._tinydng_web_error_status(handle);
        document.close();
        throw statusError(status, message);
      }
      return document;
    },
  };
}

export { STATUS };
