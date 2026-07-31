const VERTEX_SOURCE = `#version 300 es
in vec2 a_position;
void main() {
  gl_Position = vec4(a_position, 0.0, 1.0);
}
`;

const FRAGMENT_SOURCE = `#version 300 es
precision highp float;
precision highp int;

uniform sampler2D u_raw;
uniform ivec2 u_imageSize;
uniform ivec2 u_outputOrigin;
uniform ivec2 u_outputSize;
uniform ivec2 u_canvasSize;
uniform ivec2 u_sourceOrigin;
uniform vec2 u_sourceScale;
uniform float u_inputScale;
uniform int u_channels;
uniform int u_isCfa;
uniform int u_patternRows;
uniform int u_patternCols;
uniform int u_pattern[16];
uniform int u_planeColor[4];
uniform float u_black;
uniform float u_white;
uniform float u_exposure;
uniform float u_shadow;
uniform float u_highlight;
uniform int u_useColorMatrix;
uniform mat3 u_colorMatrix;
uniform mat3 u_xyzToSrgb;
uniform vec3 u_asShotNeutral;

out vec4 out_color;

ivec2 sourceForOutput() {
  float viewportY = float(u_canvasSize.y - u_outputOrigin.y - u_outputSize.y);
  vec2 localPixel = vec2(floor(gl_FragCoord.x) - float(u_outputOrigin.x),
                         floor(gl_FragCoord.y) - viewportY);
  vec2 outputPixel = vec2(u_outputOrigin) +
                     vec2(localPixel.x,
                          float(u_outputSize.y) - localPixel.y - 1.0) +
                     vec2(0.5);
  ivec2 source = ivec2(floor(outputPixel * u_sourceScale));
  return clamp(source, ivec2(0), u_imageSize - ivec2(1));
}

float rawAt(ivec2 point) {
  point = clamp(point, ivec2(0), u_imageSize - ivec2(1));
  ivec2 local = point - u_sourceOrigin;
  return texelFetch(u_raw, local, 0).r * u_inputScale;
}

vec3 directAt(ivec2 point) {
  point = clamp(point, ivec2(0), u_imageSize - ivec2(1));
  ivec2 local = point - u_sourceOrigin;
  vec4 rawSample = texelFetch(u_raw, local, 0) * u_inputScale;
  if (u_channels <= 1) return vec3(rawSample.r);
  if (u_channels == 2) return vec3(rawSample.r, rawSample.g, rawSample.g);
  return rawSample.rgb;
}

int cfaColor(ivec2 point) {
  point = clamp(point, ivec2(0), u_imageSize - ivec2(1));
  int row = point.y % u_patternRows;
  int col = point.x % u_patternCols;
  int plane = clamp(u_pattern[row * u_patternCols + col], 0, 3);
  return clamp(u_planeColor[plane], 0, 2);
}

vec3 demosaic(ivec2 center) {
  vec3 sum = vec3(0.0);
  vec3 count = vec3(0.0);
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      int color = cfaColor(center + ivec2(dx, dy));
      float value = rawAt(center + ivec2(dx, dy));
      if (color == 0) { sum.r += value; count.r += 1.0; }
      if (color == 1) { sum.g += value; count.g += 1.0; }
      if (color == 2) { sum.b += value; count.b += 1.0; }
    }
  }
  return sum / max(count, vec3(1.0));
}

vec3 toneMap(vec3 color) {
  float denominator = max(u_white - u_black, 1.0);
  color = max((color - u_black) / denominator, vec3(0.0));
  if (u_useColorMatrix != 0) {
    color /= max(u_asShotNeutral, vec3(0.0001));
    color = u_xyzToSrgb * (u_colorMatrix * color);
  }
  color *= exp2(u_exposure);
  color = max(color, vec3(0.0));
  color = color / (vec3(1.0) + u_highlight * max(color - vec3(1.0), vec3(0.0)));
  color = pow(color, vec3(1.0 - 0.5 * clamp(u_shadow, 0.0, 1.0)));
  color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
  return color;
}

void main() {
  ivec2 source = sourceForOutput();
  vec3 color = u_isCfa != 0 ? demosaic(source) : directAt(source);
  out_color = vec4(toneMap(color), 1.0);
}
`;

const XYZ_TO_SRGB = [
  3.2406, -1.5372, -0.4986,
  -0.9689, 1.8758, 0.0415,
  0.0557, -0.2040, 1.0570,
];

function compile(gl, type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(`WebGL shader compile failed: ${message}`);
  }
  return shader;
}

function createProgram(gl) {
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, VERTEX_SOURCE));
  gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, FRAGMENT_SOURCE));
  gl.bindAttribLocation(program, 0, "a_position");
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(program);
    gl.deleteProgram(program);
    throw new Error(`WebGL program link failed: ${message}`);
  }
  return program;
}

function transposeMatrix(matrix) {
  return new Float32Array([
    matrix[0], matrix[3], matrix[6],
    matrix[1], matrix[4], matrix[7],
    matrix[2], matrix[5], matrix[8],
  ]);
}

function typedSource(info, data) {
  if (info.sampleFormat === 3) {
    return new Float32Array(data.buffer, data.byteOffset, data.byteLength / 4);
  }
  if (info.bitsPerSampleDecoded > 8) {
    return new Uint16Array(data.buffer, data.byteOffset, data.byteLength / 2);
  }
  return data;
}

function sourceFormat(gl, info) {
  const float = info.sampleFormat === 3;
  const channels = info.samplesPerPixel > 1 ? 4 : 1;
  if (float) {
    return {
      internal: channels === 1 ? gl.R32F : gl.RGBA32F,
      format: channels === 1 ? gl.RED : gl.RGBA,
      type: gl.FLOAT,
      inputScale: 1,
      channels,
      TypedArray: Float32Array,
    };
  }
  if (info.bitsPerSampleDecoded > 8) {
    /* WebGL2 has no portable normalized R16/RGBA16 texture format. Convert
       each uploaded tile to float instead of relying on implementation
       specific constants; the tile remains bounded by MAX_TEXTURE_SIZE. */
    return {
      internal: channels === 1 ? gl.R32F : gl.RGBA32F,
      format: channels === 1 ? gl.RED : gl.RGBA,
      type: gl.FLOAT,
      inputScale: 1,
      channels,
      TypedArray: Float32Array,
    };
  }
  return {
    internal: channels === 1 ? gl.R8 : gl.RGBA8,
    format: channels === 1 ? gl.RED : gl.RGBA,
    type: gl.UNSIGNED_BYTE,
    inputScale: 255,
    channels,
    TypedArray: Uint8Array,
  };
}

function tileData(info, source, bounds, format, reusable) {
  const { x, y, width, height } = bounds;
  const channels = info.samplesPerPixel;
  const size = width * height * format.channels;
  const destination = reusable && reusable.length >= size
    ? reusable
    : new format.TypedArray(size);
  const fill = info.sampleFormat === 3 ? 1 : (info.bitsPerSampleDecoded > 8 ? 65535 : 255);
  for (let row = 0; row < height; row += 1) {
    const sourceStart = ((y + row) * info.width + x) * channels;
    const destinationStart = row * width * format.channels;
    if (channels === 1) {
      destination.set(source.subarray(sourceStart, sourceStart + width), destinationStart);
      continue;
    }
    for (let col = 0; col < width; col += 1) {
      const sourcePixel = sourceStart + col * channels;
      const destinationPixel = destinationStart + col * 4;
      destination[destinationPixel] = source[sourcePixel];
      destination[destinationPixel + 1] = source[sourcePixel + 1] ?? source[sourcePixel];
      destination[destinationPixel + 2] = source[sourcePixel + 2] ?? source[sourcePixel];
      destination[destinationPixel + 3] = channels > 3 ? source[sourcePixel + 3] : fill;
    }
  }
  return destination.subarray(0, size);
}

function sourceBounds(info, outputX, outputY, outputWidth, outputHeight, scaleX, scaleY) {
  const x = Math.max(0, Math.floor(outputX * scaleX) - 1);
  const y = Math.max(0, Math.floor(outputY * scaleY) - 1);
  const right = Math.min(info.width - 1,
    Math.floor((outputX + outputWidth - 1) * scaleX) + 1);
  const bottom = Math.min(info.height - 1,
    Math.floor((outputY + outputHeight - 1) * scaleY) + 1);
  return { x, y, width: right - x + 1, height: bottom - y + 1 };
}

export class RawDeveloper {
  constructor(canvas) {
    this.canvas = canvas;
    this.gl = canvas.getContext("webgl2", {
      alpha: false,
      antialias: false,
      depth: false,
      preserveDrawingBuffer: false,
    });
    if (!this.gl) throw new Error("WebGL2 is unavailable");
    this.program = createProgram(this.gl);
    this.texture = this.gl.createTexture();
    this.vertexArray = this.gl.createVertexArray();
    this.vertexBuffer = this.gl.createBuffer();
    this.gl.bindVertexArray(this.vertexArray);
    this.gl.bindBuffer(this.gl.ARRAY_BUFFER, this.vertexBuffer);
    this.gl.bufferData(this.gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), this.gl.STATIC_DRAW);
    this.gl.enableVertexAttribArray(0);
    this.gl.vertexAttribPointer(0, 2, this.gl.FLOAT, false, 0, 0);
    this.uniform = Object.fromEntries([
      "u_raw", "u_imageSize", "u_outputOrigin", "u_outputSize",
      "u_canvasSize", "u_sourceOrigin", "u_sourceScale", "u_inputScale", "u_channels",
      "u_isCfa", "u_patternRows", "u_patternCols", "u_pattern[0]",
      "u_planeColor[0]", "u_black", "u_white", "u_exposure", "u_shadow",
      "u_highlight", "u_useColorMatrix", "u_colorMatrix", "u_xyzToSrgb",
      "u_asShotNeutral",
    ].map((name) => [name, this.gl.getUniformLocation(this.program, name)]));
    this.maxTextureSize = this.gl.getParameter(this.gl.MAX_TEXTURE_SIZE);
  }

  get available() {
    return Boolean(this.gl);
  }

  async render(decoded, controls, progress = () => {}) {
    const { info, data } = decoded;
    const gl = this.gl;
    if (info.samplesPerPixel > 4 ||
        (info.sampleFormat !== 1 && info.sampleFormat !== 3)) {
      throw new Error("GPU preview supports up to four unsigned integer/float channels");
    }
    if (info.sampleFormat !== 3 && info.bitsPerSampleDecoded > 16) {
      throw new Error("GPU preview supports integer samples up to 16 bits");
    }
    const source = typedSource(info, data);
    const format = sourceFormat(gl, info);

    const displayScale = Math.min(1, 2048 / Math.max(info.width, info.height));
    const outputWidth = Math.max(1, Math.round(info.width * displayScale));
    const outputHeight = Math.max(1, Math.round(info.height * displayScale));
    const scaleX = info.width / outputWidth;
    const scaleY = info.height / outputHeight;
    const sourceLimit = Math.max(3, this.maxTextureSize - 2);
    const tileWidth = Math.max(1, Math.min(1024, Math.floor(sourceLimit / scaleX) - 2));
    const tileHeight = Math.max(1, Math.min(1024, Math.floor(sourceLimit / scaleY) - 2));
    const patternElements = info.cfaPatternRows * info.cfaPatternCols;
    const isCfa = Boolean(info.cfa && info.cfaPatternRows > 0 &&
      info.cfaPatternCols > 0 && patternElements <= 16 &&
      info.cfaPatternSize >= patternElements);
    const useMatrix = Boolean(controls.colorCorrection && info.colorMatrixPresent && isCfa);
    const black = info.blackLevelPresent ? info.blackLevel : 0;
    const white = info.whiteLevelPresent
      ? info.whiteLevel
      : (info.sampleFormat === 3 ? 1 : (2 ** info.bitsPerSample) - 1);
    const colorMatrix = info.colorMatrixPresent ? info.colorMatrix : [1, 0, 0, 0, 1, 0, 0, 0, 1];
    const neutral = info.asShotNeutralPresent ? info.asShotNeutral : [1, 1, 1];

    this.canvas.width = outputWidth;
    this.canvas.height = outputHeight;
    gl.useProgram(this.program);
    gl.bindVertexArray(this.vertexArray);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.uniform1i(this.uniform.u_raw, 0);
    gl.uniform2i(this.uniform.u_imageSize, info.width, info.height);
    gl.uniform2i(this.uniform.u_canvasSize, outputWidth, outputHeight);
    gl.uniform1f(this.uniform.u_inputScale, format.inputScale);
    gl.uniform1i(this.uniform.u_channels, info.samplesPerPixel);
    gl.uniform1i(this.uniform.u_isCfa, isCfa ? 1 : 0);
    gl.uniform1i(this.uniform.u_patternRows, isCfa ? info.cfaPatternRows : 1);
    gl.uniform1i(this.uniform.u_patternCols, isCfa ? info.cfaPatternCols : 1);
    gl.uniform1iv(this.uniform["u_pattern[0]"], new Int32Array(isCfa ? info.cfaPattern : [0]));
    gl.uniform1iv(this.uniform["u_planeColor[0]"], new Int32Array(isCfa ? info.planeColor : [0, 1, 2, 0]));
    gl.uniform1f(this.uniform.u_black, black);
    gl.uniform1f(this.uniform.u_white, white);
    gl.uniform1f(this.uniform.u_exposure, controls.exposure);
    gl.uniform1f(this.uniform.u_shadow, controls.shadow);
    gl.uniform1f(this.uniform.u_highlight, controls.highlight);
    gl.uniform1i(this.uniform.u_useColorMatrix, useMatrix ? 1 : 0);
    gl.uniformMatrix3fv(this.uniform.u_colorMatrix, false, transposeMatrix(colorMatrix));
    gl.uniformMatrix3fv(this.uniform.u_xyzToSrgb, false, transposeMatrix(XYZ_TO_SRGB));
    gl.uniform3fv(this.uniform.u_asShotNeutral, new Float32Array(neutral));
    gl.clearColor(0, 0, 0, 1);
    gl.viewport(0, 0, outputWidth, outputHeight);
    gl.clear(gl.COLOR_BUFFER_BIT);

    let tileBuffer;
    for (let outputY = 0; outputY < outputHeight; outputY += tileHeight) {
      const height = Math.min(tileHeight, outputHeight - outputY);
      for (let outputX = 0; outputX < outputWidth; outputX += tileWidth) {
        const width = Math.min(tileWidth, outputWidth - outputX);
        const bounds = sourceBounds(info, outputX, outputY, width, height, scaleX, scaleY);
        if (bounds.width > this.maxTextureSize || bounds.height > this.maxTextureSize) {
          throw new Error("Image tile exceeds the WebGL2 texture limit");
        }
        const tileSize = bounds.width * bounds.height * format.channels;
        if (!tileBuffer || !(tileBuffer instanceof format.TypedArray) ||
            tileBuffer.length < tileSize) {
          tileBuffer = new format.TypedArray(tileSize);
        }
        const tile = tileData(info, source, bounds, format, tileBuffer);
        gl.texImage2D(gl.TEXTURE_2D, 0, format.internal, bounds.width, bounds.height,
          0, format.format, format.type, tile);
        gl.uniform2i(this.uniform.u_outputOrigin, outputX, outputY);
        gl.uniform2i(this.uniform.u_outputSize, width, height);
        gl.uniform2i(this.uniform.u_sourceOrigin, bounds.x, bounds.y);
        gl.uniform2f(this.uniform.u_sourceScale, scaleX, scaleY);
        gl.viewport(outputX, outputHeight - outputY - height, width, height);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      }
      if (progress(Math.min(1, (outputY + tileHeight) / outputHeight)) === false) {
        return;
      }
      await new Promise((resolve) => requestAnimationFrame(resolve));
    }
    gl.flush();
  }
}
