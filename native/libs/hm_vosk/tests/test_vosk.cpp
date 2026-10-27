// hm_vosk unit tests. Parity with the Python VoskBackend on real speech: native/tests/test_perception_partial.py.
#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>

#include <gtest/gtest.h>

#include "hm/vosk.hpp"

namespace {

std::shared_ptr<hm::vosk::Model> shared_model() {
  static std::shared_ptr<hm::vosk::Model> model = [] {
    std::shared_ptr<hm::vosk::Model> loaded;
    if (std::string(HM_VOSK_TEST_LIBRARY).empty() || !std::filesystem::is_directory(HM_VOSK_TEST_MODEL)) return loaded;
    loaded = std::make_shared<hm::vosk::Model>(hm::vosk::Library::load(HM_VOSK_TEST_LIBRARY), HM_VOSK_TEST_MODEL);
    return loaded;
  }();
  return model;
}

std::vector<float> noise(std::size_t samples, float amplitude, unsigned seed) {
  std::mt19937 engine(seed);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  std::vector<float> out(samples);
  for (float& value : out) value = amplitude * normal(engine);
  return out;
}

}  // namespace

TEST(VoskText, PythonStrip) {
  EXPECT_EQ(hm::vosk::py_strip("  불 켜  \n"), "불 켜");
  EXPECT_EQ(hm::vosk::py_strip("\xe3\x80\x80가\xc2\xa0"), "가");  // U+3000 ideographic space, U+00A0
  EXPECT_EQ(hm::vosk::py_strip("\x1c\x1f a b \x0b"), "a b");
  EXPECT_EQ(hm::vosk::py_strip(""), "");
  EXPECT_EQ(hm::vosk::py_strip(" \t "), "");
  EXPECT_EQ(hm::vosk::py_strip("\xe2\x80\x8b x"), "\xe2\x80\x8b x");  // U+200B is not whitespace in Python
}

TEST(VoskText, Pcm16IsLosslessForPcm16Sources) {
  std::vector<float> samples;
  for (int value = -32768; value <= 32767; value += 7) samples.push_back(static_cast<float>(value) / 32768.0f);
  samples.push_back(32767.0f / 32768.0f);
  std::vector<std::int16_t> pcm;
  hm::vosk::to_pcm16(samples.data(), samples.size(), pcm);
  ASSERT_EQ(pcm.size(), samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    ASSERT_EQ(static_cast<float>(pcm[i]) / 32768.0f, samples[i]) << i;
  }
  const float out_of_range[] = {1.5f, -1.5f, 1.0f, -1.0f};
  hm::vosk::to_pcm16(out_of_range, 4, pcm);
  EXPECT_EQ(pcm, (std::vector<std::int16_t>{32767, -32768, 32767, -32768}));
}

TEST(VoskLibrary, MissingLibraryIsAClearError) {
  try {
    hm::vosk::Library::load("/nonexistent/libvosk.so");
    FAIL() << "expected an error";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("cannot load /nonexistent/libvosk.so"), std::string::npos);
  }
}

TEST(VoskLibrary, MissingModelFolder) {
  if (std::string(HM_VOSK_TEST_LIBRARY).empty()) GTEST_SKIP() << "no prebuilt libvosk for this architecture";
  auto library = hm::vosk::Library::load(HM_VOSK_TEST_LIBRARY);
  EXPECT_THROW(hm::vosk::Model(library, "/nonexistent/model"), std::runtime_error);
}

TEST(VoskStream, ChunkingIndependentOfHowAudioArrives) {
  auto model = shared_model();
  if (!model) GTEST_SKIP() << "libvosk or the Korean model is missing";
  EXPECT_EQ(model->version(), "vosk-model-small-ko-0.22");
  std::vector<float> audio = noise(16000 * 3, 0.05f, 3);
  for (std::size_t i = 16000; i < 32000; ++i) {  // a vowel-like buzz in the middle second
    audio[i] += 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 180.0 * static_cast<double>(i) / 16000.0) +
                                          0.5 * std::sin(2.0 * std::numbers::pi * 720.0 * static_cast<double>(i) / 16000.0));
  }
  auto run = [&](std::size_t feed) {
    hm::vosk::PartialStream stream(model, 1600);
    std::vector<std::string> partials;
    for (std::size_t offset = 0; offset < audio.size(); offset += feed) {
      for (auto& text : stream.accept(audio.data() + offset, std::min(feed, audio.size() - offset))) {
        partials.push_back(text);
      }
    }
    for (auto& text : stream.flush()) partials.push_back(text);
    EXPECT_EQ(stream.chunks(), audio.size() / 1600 + (audio.size() % 1600 != 0));
    return std::make_pair(partials, stream.final_text());
  };
  const auto blocks = run(1600);
  EXPECT_EQ(run(4800 + 320), blocks);  // VAD onset hands over pre-roll + frames, then 100 ms blocks
  EXPECT_EQ(run(audio.size()), blocks);
  for (std::size_t i = 1; i < blocks.first.size(); ++i) EXPECT_NE(blocks.first[i], blocks.first[i - 1]);
}

TEST(VoskStream, SilenceGivesNoPartial) {
  auto model = shared_model();
  if (!model) GTEST_SKIP() << "libvosk or the Korean model is missing";
  const std::vector<float> silence(16000 * 2, 0.0f);
  hm::vosk::PartialStream stream(model, 1600);
  EXPECT_TRUE(stream.accept(silence.data(), silence.size()).empty());
  EXPECT_TRUE(stream.flush().empty());
  EXPECT_EQ(stream.chunks(), 20u);
  EXPECT_EQ(stream.chunk_ms().size(), 20u);
}
