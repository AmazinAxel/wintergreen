#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ZipReader.h"

namespace wintergreen {

// Decoded image: 8-bit grayscale (255 = white), row-major, already scaled.
// Quantising to the panel's levels is the caller's (quantise_level and the
// packers below), so every consumer dithers once, at its final size.
struct DecodedImage {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<uint8_t> data;  // width bytes per row

  size_t stride() const {
    return width;
  }
  size_t data_size() const {
    return stride() * height;
  }

  // Gray at (x, y); white for out-of-bounds coords.
  uint8_t pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height)
      return 255;
    return data[static_cast<size_t>(y) * stride() + static_cast<size_t>(x)];
  }
};

// ---------------------------------------------------------------------------
// Panel levels
// ---------------------------------------------------------------------------
//
// What the panel's gray and light levels reflect, 0 = black .. 1 = white: the
// targets the quantiser picks between. Guesses until measured on the device
// (docs/display.md "Waveform tuning"); tools/make_sleep_image.py uses the same.
inline constexpr float kGrayLevel = 0.5f;
inline constexpr float kLightLevel = 0.75f;

// Level (0 white, 1 light, 2 gray, 3 black) for gray `g` at (x, y), blue-noise
// dithered between the two levels around it. `levels` 3 = white, gray, black
// (normal updates: images, covers); 4 adds light (full refreshes: the sleep
// cover). Flat input at a level's own tone stays exact; mean tone is kept.
int quantise_level(uint8_t g, int x, int y, int levels);

// Pack a w x h gray image as two planes (hi = gray or black, lo = black; set =
// ink, MSB first), `levels` as above:
//   rows: row r = ceil(w/8) hi bytes then ceil(w/8) lo bytes
//   cols: column c = ceil(h/8) hi bytes then ceil(h/8) lo bytes, top MSB
// `out` must hold 2 * ceil(w/8) * h (rows) or 2 * ceil(h/8) * w (cols) bytes.
void pack_rows(const uint8_t* gray, int w, int h, int levels, uint8_t* out);
void pack_cols(const uint8_t* gray, int w, int h, int levels, uint8_t* out);

// The sleep frame (CoverPaths.h, 4 levels) of a kSleepCoverW x kSleepCoverH
// portrait gray image; `frame` holds kSleepFrameBytes. Same format as
// tools/make_sleep_image.py writes for the wordmark.
void pack_sleep_frame(const uint8_t* gray, uint8_t* frame);

enum class ImageFormat {
  Unknown,
  Jpeg,
  Png,
};

// Errors from image operations.
enum class ImageError {
  Ok = 0,
  UnsupportedFormat,
  InvalidData,
  ReadError,
  TooLarge,
};

// Guess format from filename extension.
ImageFormat guess_format(const char* filename);

// Guess format from magic bytes (first 2+ bytes of data).
ImageFormat guess_format_from_magic(const uint8_t* data, size_t size);

// Read image dimensions from raw data without full decoding.
// Works for JPEG (scans for SOF marker) and PNG (reads IHDR).
ImageError read_image_size(const uint8_t* data, size_t size, uint16_t& width, uint16_t& height);

// Streaming image size parser — ~32 bytes of state, no heap allocation.
// Feed decompressed data chunks until feed() returns true, then check ok().
// Handles JPEG (scans past APPn/EXIF/ICC segments to SOF) and PNG (reads IHDR).
// PNG needs 24 bytes; JPEG SOF varies (typically 1-25KB with EXIF/ICC profiles).
struct ImageSizeStream {
  ImageSizeStream() = default;

  // Feed a data chunk. Returns true when done (success OR unsupported format).
  // After true: call ok() to check success, width()/height() for dimensions.
  bool feed(const uint8_t* data, size_t size);

  bool ok() const {
    return state_ == State::Done;
  }
  uint16_t width() const {
    return w_;
  }
  uint16_t height() const {
    return h_;
  }

 private:
  enum class State : uint8_t {
    Detect,    // inspect first 2 bytes to determine format
    PngSkip,   // skip bytes 2–15 (sig tail + IHDR length + "IHDR" tag)
    PngSize,   // read 8 bytes: width[4] height[4]
    JpegScan,  // scan forward for 0xFF marker byte
    JpegMark,  // read marker byte following 0xFF
    JpegLen1,  // read segment length high byte
    JpegLen2,  // read segment length low byte
    JpegSkip,  // skip segment payload bytes
    JpegSof,   // accumulate 7 SOF payload bytes (len16 + prec8 + height16 + width16)
    Done,
    Error,
  };

  State state_ = State::Detect;
  uint8_t detect_n_ = 0;
  uint8_t detect_[2] = {};
  uint8_t skip_left_ = 0;  // PNG: bytes remaining in PngSkip
  uint8_t size_[8] = {};   // PNG: accumulates width+height bytes
  uint8_t size_n_ = 0;
  uint32_t seg_left_ = 0;  // JPEG: bytes remaining to skip in current segment
  uint8_t sof_[7] = {};    // JPEG: SOF payload buffer
  uint8_t sof_n_ = 0;
  uint16_t w_ = 0, h_ = 0;
};

// Compute aspect-ratio-preserving scaled dimensions.
// Returns dimensions that fit within max_w × max_h.
void scaled_size(uint16_t raw_w, uint16_t raw_h, uint16_t max_w, uint16_t max_h, uint16_t& out_w, uint16_t& out_h);

// Read image dimensions from raw data without full decoding.
// Uses ImageSizeStream internally. Returns true if successful.
bool get_image_size(const uint8_t* data, size_t size, uint16_t& out_w, uint16_t& out_h);

// Callback invoked for each output row during streaming decode.
// y: 0-based row index within the image.
// data: 8-bit gray row, width bytes.
// width: image width in pixels.
struct ImageRowSink {
  void (*emit_row)(void* ctx, uint16_t y, const uint8_t* data, uint16_t width) = nullptr;
  void* ctx = nullptr;
};

// Callback invoked per pixel during Adam7 interlaced PNG decode.
// Pixels arrive out of row order; the callback must support random writes.
// x, y: output pixel coordinates. gray: 255 = white.
struct ImagePixelSink {
  void (*set_pixel)(void* ctx, uint16_t x, uint16_t y, uint8_t gray) = nullptr;
  void* ctx = nullptr;
};

// Optional 256-entry tone map applied to each grayscale sample on output.
// nullptr = identity. Used by the sleep cover to contrast-stretch a
// low-contrast cover into the panel's range; see build_stretch_lut.
using ToneLut = const uint8_t*;

// Build a contrast-stretch LUT from a grayscale histogram.
// hist: 256 bin counts. clip_frac: fraction of pixels allowed to saturate at
// each end (e.g. 0.005 = 0.5%). Returns false when the image already spans a
// usable range, in which case lut is left untouched and should not be applied.
bool build_stretch_lut(const uint32_t hist[256], double clip_frac, uint8_t lut[256]);

// Accumulates a grayscale histogram of the samples a decode outputs. Non-null = collect; the decode is otherwise unaffected. This is how
// the sleep cover's probe pass learns the source tone range without a separate
// grayscale decode path.
struct ToneHistogram {
  uint32_t bins[256] = {};
};

// Decode a JPEG image by streaming directly from a ZIP entry.
// work_buf must be >= ZipEntryInput::kMinWorkBufSize for deflate entries;
// if nullptr (or too small), a buffer is heap-allocated internally.
// scale_to_fill: if true, scale up to fill max_w×max_h even when the source
// is smaller (aspect-ratio-preserving). Default false = cap to max, keep
// natural size for small images.
// sink: if non-null, each dithered row is emitted via the callback instead of
// being stored in out.data. out.width/height are still set.
ImageError decode_jpeg_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                  DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                  bool scale_to_fill = false, ImageRowSink* sink = nullptr,
                                  ToneLut tone_lut = nullptr, ToneHistogram* tone_hist = nullptr);

// Decode a PNG image by streaming directly from a ZIP entry.
// Same work_buf, scale_to_fill, and sink semantics as decode_jpeg_from_entry.
// pixel_sink: if non-null and the PNG is Adam7 interlaced, pixels are emitted
// via this random-access callback instead of row_sink (no output buffer needed).
ImageError decode_png_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                 DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                 bool scale_to_fill = false, ImageRowSink* sink = nullptr,
                                 ImagePixelSink* pixel_sink = nullptr,
                                 ToneLut tone_lut = nullptr, ToneHistogram* tone_hist = nullptr);

// Decode a JPEG or PNG image by streaming from a ZIP entry.
// Detects format from the first bytes of the entry.
// work_buf: optional caller-provided buffer (>= ZipEntryInput::kMinWorkBufSize).
// sink: if non-null, rows are emitted via callback (no out.data allocation).
// pixel_sink: if non-null and the PNG is Adam7 interlaced, pixels are emitted
// via this random-access callback instead of buffering the full output.
ImageError decode_image_from_entry(IZipFile& file, const ZipEntry& entry, uint16_t max_w, uint16_t max_h,
                                   DecodedImage& out, uint8_t* work_buf = nullptr, size_t work_buf_size = 0,
                                   bool scale_to_fill = false, ImageRowSink* sink = nullptr,
                                   ImagePixelSink* pixel_sink = nullptr,
                                   ToneLut tone_lut = nullptr, ToneHistogram* tone_hist = nullptr);

}  // namespace wintergreen
