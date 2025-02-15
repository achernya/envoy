#include "envoy/compression/compressor/factory.h"
#include "envoy/extensions/filters/http/compressor/v3/compressor.pb.h"

#include "source/extensions/compression/gzip/compressor/zlib_compressor_impl.h"
#include "source/extensions/compression/zstd/compressor/zstd_compressor_impl.h"
#include "source/extensions/filters/http/compressor/compressor_filter.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/stats/mocks.h"

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"

using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Compressor {

class MockGzipCompressorFactory : public Envoy::Compression::Compressor::CompressorFactory {
public:
  MockGzipCompressorFactory(
      Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionLevel level,
      Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionStrategy strategy,
      int64_t window_bits, uint64_t memory_level)
      : level_(level), strategy_(strategy), window_bits_(window_bits), memory_level_(memory_level) {
  }

  Envoy::Compression::Compressor::CompressorPtr createCompressor() override {
    auto compressor =
        std::make_unique<Compression::Gzip::Compressor::ZlibCompressorImpl>(chunk_size_);
    compressor->init(level_, strategy_, window_bits_, memory_level_);
    return compressor;
  }

  const std::string& statsPrefix() const override { CONSTRUCT_ON_FIRST_USE(std::string, "gzip."); }
  const std::string& contentEncoding() const override {
    return Http::CustomHeaders::get().ContentEncodingValues.Gzip;
  }

private:
  const Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionLevel level_;
  const Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionStrategy strategy_;
  const int64_t window_bits_;
  const uint64_t memory_level_;
  const uint64_t chunk_size_{4096};
};

class MockZstdCompressorFactory : public Envoy::Compression::Compressor::CompressorFactory {
public:
  MockZstdCompressorFactory(uint32_t level, uint32_t strategy)
      : level_(level), strategy_(strategy) {}

  Envoy::Compression::Compressor::CompressorPtr createCompressor() override {
    return std::make_unique<Compression::Zstd::Compressor::ZstdCompressorImpl>(
        level_, enable_checksum_, strategy_, cdict_manager_, chunk_size_);
  }

  const std::string& statsPrefix() const override { CONSTRUCT_ON_FIRST_USE(std::string, "zstd."); }
  const std::string& contentEncoding() const override {
    return Http::CustomHeaders::get().ContentEncodingValues.Zstd;
  }

private:
  const uint32_t level_;
  const uint32_t strategy_;
  const bool enable_checksum_{};
  Compression::Zstd::Compressor::ZstdCDictManagerPtr cdict_manager_{nullptr};
  const uint64_t chunk_size_{4096};
};

using CompressionParams = std::tuple<int64_t, uint64_t, int64_t, uint64_t>;

CompressorFilterConfigSharedPtr makeGzipConfig(Stats::IsolatedStoreImpl& stats,
                                               testing::NiceMock<Runtime::MockLoader>& runtime,
                                               CompressionParams params) {

  envoy::extensions::filters::http::compressor::v3::Compressor compressor;

  const auto level =
      static_cast<Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionLevel>(
          std::get<0>(params));
  const auto strategy =
      static_cast<Compression::Gzip::Compressor::ZlibCompressorImpl::CompressionStrategy>(
          std::get<1>(params));
  const auto window_bits = std::get<2>(params);
  const auto memory_level = std::get<3>(params);
  Envoy::Compression::Compressor::CompressorFactoryPtr compressor_factory =
      std::make_unique<MockGzipCompressorFactory>(level, strategy, window_bits, memory_level);
  CompressorFilterConfigSharedPtr config = std::make_shared<CompressorFilterConfig>(
      compressor, "test.", stats, runtime, std::move(compressor_factory));

  return config;
}

CompressorFilterConfigSharedPtr makeZstdConfig(Stats::IsolatedStoreImpl& stats,
                                               testing::NiceMock<Runtime::MockLoader>& runtime,
                                               CompressionParams params) {

  envoy::extensions::filters::http::compressor::v3::Compressor compressor;

  const auto level = std::get<0>(params);
  const auto strategy = std::get<1>(params);
  Envoy::Compression::Compressor::CompressorFactoryPtr compressor_factory =
      std::make_unique<MockZstdCompressorFactory>(level, strategy);
  CompressorFilterConfigSharedPtr config = std::make_shared<CompressorFilterConfig>(
      compressor, "test.", stats, runtime, std::move(compressor_factory));

  return config;
}

static constexpr uint64_t TestDataSize = 122880;

Buffer::OwnedImpl generateTestData() {
  Buffer::OwnedImpl data;
  TestUtility::feedBufferWithRandomCharacters(data, TestDataSize);
  return data;
}

const Buffer::OwnedImpl& testData() {
  CONSTRUCT_ON_FIRST_USE(Buffer::OwnedImpl, generateTestData());
}

static std::vector<Buffer::OwnedImpl> generateChunks(const uint64_t chunk_count,
                                                     const uint64_t chunk_size) {
  std::vector<Buffer::OwnedImpl> vec;
  vec.reserve(chunk_count);

  const auto& test_data = testData();
  uint64_t added = 0;

  for (uint64_t i = 0; i < chunk_count; ++i) {
    Buffer::OwnedImpl chunk;
    std::unique_ptr<char[]> data(new char[chunk_size]);

    test_data.copyOut(added, chunk_size, data.get());
    chunk.add(absl::string_view(data.get(), chunk_size));
    vec.push_back(std::move(chunk));

    added += chunk_size;
  }

  return vec;
}

struct Result {
  uint64_t total_uncompressed_bytes = 0;
  uint64_t total_compressed_bytes = 0;
};

enum class CompressorLibs { Gzip, Zstd };

static Result compressWith(enum CompressorLibs lib, std::vector<Buffer::OwnedImpl>&& chunks,
                           CompressionParams params,
                           NiceMock<Http::MockStreamDecoderFilterCallbacks>& decoder_callbacks,
                           benchmark::State& state) {
  auto start = std::chrono::high_resolution_clock::now();
  Stats::IsolatedStoreImpl stats;
  testing::NiceMock<Runtime::MockLoader> runtime;
  CompressorFilterConfigSharedPtr config;
  std::string compressor = "";
  if (lib == CompressorLibs::Gzip) {
    config = makeGzipConfig(stats, runtime, params);
    compressor = "gzip";
  } else if (lib == CompressorLibs::Zstd) {
    config = makeZstdConfig(stats, runtime, params);
    compressor = "zstd";
  }

  ON_CALL(runtime.snapshot_, featureEnabled("test.filter_enabled", 100))
      .WillByDefault(Return(true));

  auto filter = std::make_unique<CompressorFilter>(config);
  filter->setDecoderFilterCallbacks(decoder_callbacks);

  Http::TestRequestHeaderMapImpl headers = {
      {":method", "get"}, {"accept-encoding", compressor}, {"content-encoding", compressor}};
  filter->decodeHeaders(headers, false);

  Http::TestResponseHeaderMapImpl response_headers = {
      {":method", "get"},
      {"content-length", "122880"},
      {"content-type", "application/json;charset=utf-8"}};
  filter->encodeHeaders(response_headers, false);

  uint64_t idx = 0;
  Result res;
  for (auto& data : chunks) {
    res.total_uncompressed_bytes += data.length();

    if (idx == (chunks.size() - 1)) {
      filter->encodeData(data, true);
    } else {
      filter->encodeData(data, false);
    }

    res.total_compressed_bytes += data.length();
    ++idx;
  }

  EXPECT_EQ(res.total_uncompressed_bytes,
            stats
                .counterFromString(
                    absl::StrCat("test.compressor..", compressor, ".total_uncompressed_bytes"))
                .value());
  EXPECT_EQ(res.total_compressed_bytes,
            stats
                .counterFromString(
                    absl::StrCat("test.compressor..", compressor, ".total_compressed_bytes"))
                .value());

  EXPECT_EQ(1U,
            stats.counterFromString(absl::StrCat("test.compressor..", compressor, ".compressed"))
                .value());
  auto end = std::chrono::high_resolution_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
  state.SetIterationTime(elapsed.count());

  return res;
}

// SPELLCHECKER(off)
/*
Running ./bazel-bin/test/extensions/filters/http/common/compressor/compressor_filter_speed_test
Run on (8 X 2300 MHz CPU s)
CPU Caches:
L1 Data 32K (x4)
L1 Instruction 32K (x4)
L2 Unified 262K (x4)
L3 Unified 6291K (x1)
Load Average: 1.82, 1.72, 1.74
***WARNING*** Library was built as DEBUG. Timings may be affected.
------------------------------------------------------------
Benchmark                  Time             CPU   Iterations
------------------------------------------------------------
....
compressFull/0/manual_time              14.1 ms         14.3 ms           48
compressFull/1/manual_time              7.06 ms         7.22 ms          104
compressFull/2/manual_time              5.17 ms         5.33 ms          123
compressFull/3/manual_time              15.4 ms         15.5 ms           45
compressFull/4/manual_time              10.1 ms         10.3 ms           69
compressFull/5/manual_time              15.8 ms         16.0 ms           40
compressFull/6/manual_time              15.3 ms         15.5 ms           42
compressFull/7/manual_time              9.91 ms         10.1 ms           71
compressFull/8/manual_time              15.8 ms         16.0 ms           45
compressChunks16384/0/manual_time       13.4 ms         13.5 ms           52
compressChunks16384/1/manual_time       6.33 ms         6.48 ms          111
compressChunks16384/2/manual_time       5.09 ms         5.27 ms          147
compressChunks16384/3/manual_time       15.1 ms         15.3 ms           46
compressChunks16384/4/manual_time       9.61 ms         9.78 ms           71
compressChunks16384/5/manual_time       14.5 ms         14.6 ms           47
compressChunks16384/6/manual_time       14.0 ms         14.1 ms           48
compressChunks16384/7/manual_time       9.20 ms         9.36 ms           76
compressChunks16384/8/manual_time       14.5 ms         14.6 ms           48
compressChunks8192/0/manual_time        14.3 ms         14.5 ms           50
compressChunks8192/1/manual_time        6.80 ms         6.96 ms          100
compressChunks8192/2/manual_time        5.21 ms         5.36 ms          135
compressChunks8192/3/manual_time        14.9 ms         15.0 ms           47
compressChunks8192/4/manual_time        9.71 ms         9.87 ms           68
compressChunks8192/5/manual_time        15.9 ms         16.1 ms           45
....
*/
// SPELLCHECKER(on)

static std::vector<CompressionParams> gzip_compression_params = {
    // Speed + Standard + Small Window + Low mem level
    {Z_BEST_SPEED, Z_DEFAULT_STRATEGY, 9, 1},

    // Speed + Standard + Med window + Med mem level
    {Z_BEST_SPEED, Z_DEFAULT_STRATEGY, 12, 5},

    // Speed + Standard + Big window + High mem level
    {Z_BEST_SPEED, Z_DEFAULT_STRATEGY, 15, 9},

    // Standard + Standard + Small window + Low mem level
    {Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY, 9, 1},

    // Standard + Standard + Med window + Med mem level
    {Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY, 12, 5},

    // Standard + Standard + High window + High mem level
    {Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY, 15, 9},

    // Best + Standard + Small window + Low mem level
    {Z_BEST_COMPRESSION, Z_DEFAULT_STRATEGY, 9, 1},

    // Best + Standard + Med window + Med mem level
    {Z_BEST_COMPRESSION, Z_DEFAULT_STRATEGY, 12, 5},

    // Best + Standard + High window + High mem level
    {Z_BEST_COMPRESSION, Z_DEFAULT_STRATEGY, 15, 9}};

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressFullWithGzip(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = gzip_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(1, 122880);
    compressWith(CompressorLibs::Gzip, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressFullWithGzip)
    ->DenseRange(0, 8, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks16384WithGzip(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = gzip_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(7, 16384);
    compressWith(CompressorLibs::Gzip, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks16384WithGzip)
    ->DenseRange(0, 8, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks8192WithGzip(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = gzip_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(15, 8192);
    compressWith(CompressorLibs::Gzip, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks8192WithGzip)
    ->DenseRange(0, 8, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks4096WithGzip(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = gzip_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(30, 4096);
    compressWith(CompressorLibs::Gzip, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks4096WithGzip)
    ->DenseRange(0, 8, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks1024WithGzip(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = gzip_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(120, 1024);
    compressWith(CompressorLibs::Gzip, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks1024WithGzip)
    ->DenseRange(0, 8, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

static std::vector<CompressionParams> zstd_compression_params = {
    // level1 + default
    {1, 0, 0, 0},

    // level2 + default
    {2, 0, 0, 0},

    // level3 + default
    {3, 0, 0, 0},

    // level4 + default
    {4, 0, 0, 0},

    // level5 + default
    {5, 0, 0, 0},

    // level6 + default
    {6, 0, 0, 0},

    // level7 + default
    {7, 0, 0, 0},

    // level8 + default
    {8, 0, 0, 0},

    // level9 + default
    {9, 0, 0, 0},

    // level10 + default
    {10, 0, 0, 0},

    // level11 + default
    {11, 0, 0, 0},

    // level12 + default
    {12, 0, 0, 0},

    // level13 + default
    {13, 0, 0, 0},

    // level14 + default
    {14, 0, 0, 0},

    // level15 + default
    {15, 0, 0, 0},

    // level16 + default
    {16, 0, 0, 0},

    // level17 + default
    {17, 0, 0, 0},

    // level18 + default
    {18, 0, 0, 0},

    // level19 + default
    {19, 0, 0, 0},

    // level20 + default
    {20, 0, 0, 0},

    // level21 + default
    {21, 0, 0, 0},

    // level22 + default
    {22, 0, 0, 0}};

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressFullWithZstd(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = zstd_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(1, 122880);
    compressWith(CompressorLibs::Zstd, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressFullWithZstd)
    ->DenseRange(0, 21, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks16384WithZstd(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = zstd_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(7, 16384);
    compressWith(CompressorLibs::Zstd, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks16384WithZstd)
    ->DenseRange(0, 21, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks8192WithZstd(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = zstd_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(15, 8192);
    compressWith(CompressorLibs::Zstd, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks8192WithZstd)
    ->DenseRange(0, 21, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks4096WithZstd(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = zstd_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(30, 4096);
    compressWith(CompressorLibs::Zstd, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks4096WithZstd)
    ->DenseRange(0, 21, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// NOLINTNEXTLINE(readability-identifier-naming)
static void compressChunks1024WithZstd(benchmark::State& state) {
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks;
  const auto idx = state.range(0);
  const auto& params = zstd_compression_params[idx];

  for (auto _ : state) { // NOLINT
    std::vector<Buffer::OwnedImpl> chunks = generateChunks(120, 1024);
    compressWith(CompressorLibs::Zstd, std::move(chunks), params, decoder_callbacks, state);
  }
}
BENCHMARK(compressChunks1024WithZstd)
    ->DenseRange(0, 21, 1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

} // namespace Compressor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
