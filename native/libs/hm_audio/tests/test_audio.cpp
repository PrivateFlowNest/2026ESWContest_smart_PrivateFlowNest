// hm_audio unit tests. VAD cases are the services/runtime tests/test_perception.py VAD tests; bit parity with
// the Python reference on real and synthetic audio is checked in native/tests/test_perception_audio.py.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "hm/audio/ptt.hpp"
#include "hm/audio/resample.hpp"
#include "hm/audio/ring.hpp"
#include "hm/audio/vad.hpp"
#include "hm/audio/wav.hpp"

namespace fs = std::filesystem;
using namespace hm::audio;

namespace {

constexpr int SR = 16000;
constexpr double kPi = 3.141592653589793;

std::vector<float> tone(double seconds, float amplitude = 0.3f, double freq = 220.0, int rate = SR) {
  std::vector<float> out(static_cast<std::size_t>(seconds * rate));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
  }
  return out;
}

std::vector<float> silence(double seconds, float amplitude = 0.0005f, unsigned seed = 0) {
  std::mt19937 engine(seed);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  std::vector<float> out(static_cast<std::size_t>(seconds * SR));
  for (float& value : out) value = amplitude * normal(engine);
  return out;
}

std::vector<float> cat(std::initializer_list<std::vector<float>> parts) {
  std::vector<float> out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

std::vector<Utterance> feed_blocks(EnergyEndpointer& endpointer, const std::vector<float>& audio, std::size_t block = 1600) {
  std::vector<Utterance> found;
  for (std::size_t offset = 0; offset < audio.size(); offset += block) {
    for (auto& u : endpointer.feed(audio.data() + offset, std::min(block, audio.size() - offset))) {
      found.push_back(std::move(u));
    }
  }
  return found;
}

fs::path temp_dir() {
  char pattern[] = "/tmp/hm-audio-test-XXXXXX";
  const char* made = ::mkdtemp(pattern);
  if (!made) throw std::runtime_error("mkdtemp failed");
  return made;
}

}  // namespace

// ---- VAD (ports of the Python tests) ---------------------------------------------------------------
TEST(Vad, FindsOneUtteranceAfterSilence) {
  EnergyEndpointer endpointer;
  const auto utterances = feed_blocks(endpointer, cat({silence(1.0), tone(1.2), silence(1.5)}));
  ASSERT_EQ(utterances.size(), 1u);
  EXPECT_EQ(utterances[0].reason, "silence");
  EXPECT_GE(utterances[0].start_sample, static_cast<std::int64_t>(0.6 * SR));
  EXPECT_LE(utterances[0].start_sample, static_cast<std::int64_t>(1.0 * SR));
  EXPECT_GE(utterances[0].seconds(), 1.2);
  EXPECT_LE(utterances[0].seconds(), 1.9);
  EXPECT_EQ(utterances[0].end_sample - utterances[0].start_sample, static_cast<std::int64_t>(utterances[0].samples.size()));
}

TEST(Vad, IgnoresShortClickAndBelowMin) {
  EnergyEndpointer endpointer;
  EXPECT_TRUE(feed_blocks(endpointer, cat({silence(1.0), tone(0.1), silence(1.0), tone(0.3), silence(1.2)})).empty());
}

TEST(Vad, SplitsTwoUtterancesAndCapsMaxLength) {
  EnergyEndpointer endpointer;
  EXPECT_EQ(feed_blocks(endpointer, cat({silence(0.5), tone(0.8), silence(1.0), tone(0.8), silence(1.0)})).size(), 2u);
  EndpointConfig config;
  config.max_ms = 3000;
  EnergyEndpointer capped_endpointer(config);
  const auto capped = feed_blocks(capped_endpointer, cat({silence(0.5), tone(10.0)}));
  ASSERT_FALSE(capped.empty());
  EXPECT_EQ(capped[0].reason, "max_length");
  EXPECT_NEAR(capped[0].seconds(), 3.0, 0.05);
}

TEST(Vad, ShortPauseDoesNotEndUtterance) {
  EnergyEndpointer endpointer;
  const auto utterances = feed_blocks(endpointer, cat({silence(0.5), tone(0.6), silence(0.4), tone(0.6), silence(1.0)}));
  ASSERT_EQ(utterances.size(), 1u);
  EXPECT_GT(utterances[0].seconds(), 1.5);
}

TEST(Vad, PushToTalkWaitsForBegin) {
  EnergyEndpointer endpointer({}, /*armed=*/false);
  EXPECT_TRUE(feed_blocks(endpointer, cat({silence(0.5), tone(1.0), silence(1.0)})).empty());
  endpointer.begin();
  const auto found = feed_blocks(endpointer, cat({silence(1.0), tone(0.8), silence(1.0)}));
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].reason, "silence");
  endpointer.begin();
  feed_blocks(endpointer, tone(0.6));
  const auto manual = endpointer.stop("manual");
  ASSERT_TRUE(manual.has_value());
  EXPECT_EQ(manual->reason, "manual");
}

// Streaming view for partial transcripts: what was handed out while the utterance was open is a prefix of
// (or, with trailing silence trimmed, extends) the utterance that goes to whisper; decisions do not change.
TEST(Vad, ActiveFramesStreamThePreRollAndTheUtterance) {
  const auto audio = cat({silence(1.0), tone(1.0), silence(1.5), tone(0.8), silence(1.5)});
  EnergyEndpointer endpointer;
  EnergyEndpointer reference;
  std::vector<float> streamed;
  std::size_t sent = 0;
  std::uint64_t activation = 0;
  std::vector<std::vector<float>> per_utterance;
  for (std::size_t offset = 0; offset < audio.size(); offset += 1600) {
    for (auto& u : endpointer.feed(audio.data() + offset, 1600)) {
      ASSERT_GE(streamed.size(), u.samples.size());  // trimmed: whisper gets a prefix of what was streamed
      EXPECT_TRUE(std::equal(u.samples.begin(), u.samples.end(), streamed.begin()));
      per_utterance.push_back(u.samples);
      streamed.clear();
      sent = 0;
    }
    if (endpointer.in_speech()) {
      if (endpointer.activations() != activation) {
        activation = endpointer.activations();
        streamed.clear();
        sent = 0;
      }
      endpointer.copy_active_frames(sent, streamed);
      sent = endpointer.active_frame_count();
      EXPECT_EQ(streamed.size(), sent * endpointer.frame_samples());
    }
  }
  EXPECT_EQ(endpointer.activations(), 2u);
  const auto expected = feed_blocks(reference, audio);  // same decisions without the streaming calls
  ASSERT_EQ(per_utterance.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(per_utterance[i], expected[i].samples);
  EnergyEndpointer idle({}, /*armed=*/false);
  std::vector<float> none;
  idle.copy_active_frames(0, none);
  EXPECT_TRUE(none.empty());
  idle.begin();
  EXPECT_EQ(idle.activations(), 1u);
}

TEST(Vad, BlockSizeDoesNotChangeDecisions) {
  const auto audio = cat({silence(0.7), tone(1.0), silence(0.9), tone(0.5, 0.2f, 440.0), silence(1.0)});
  EnergyEndpointer a;
  EnergyEndpointer b;
  const auto first = feed_blocks(a, audio, 1600);
  const auto second = feed_blocks(b, audio, 333);
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].start_sample, second[i].start_sample);
    EXPECT_EQ(first[i].samples, second[i].samples);
  }
}

TEST(Vad, ConfigValidation) {
  EXPECT_EQ(EndpointConfig::from_json(nullptr).max_ms, 8000);
  const auto config = EndpointConfig::from_json({{"start_ms", 200}, {"threshold_db", -40}, {"noise_margin_db", 12.5}});
  EXPECT_EQ(config.threshold_db, -40.0);
  EXPECT_EQ(config.noise_margin_db, 12.5);
  EXPECT_THROW(EndpointConfig::from_json({{"bogus", 1}}), std::invalid_argument);
  EXPECT_THROW(EndpointConfig::from_json({{"start_ms", 200.5}}), std::invalid_argument);
  EXPECT_THROW(EndpointConfig::from_json({{"min_ms", 9000}}), std::invalid_argument);
  EXPECT_THROW(EndpointConfig::from_json({{"start_ratio", 0}}), std::invalid_argument);
  EXPECT_THROW(EndpointConfig::from_json({{"start_ms", 10}}), std::invalid_argument);
}

// ---- resampler -------------------------------------------------------------------------------------
TEST(Resample, LengthsAndPassthrough) {
  const auto x = tone(1.0, 0.5f, 1000.0, 48000);
  EXPECT_EQ(resample_poly(x, 1, 3).size(), 16000u);
  EXPECT_EQ(resample_poly(std::vector<float>(44101, 0.0f), 160, 441).size(), 16001u);  // ceil
  EXPECT_EQ(resample_poly(x, 2, 2), x);
  int up = 0;
  int down = 0;
  rational_factors(44100, 16000, up, down);
  EXPECT_EQ(up, 160);
  EXPECT_EQ(down, 441);
}

TEST(Resample, KeepsInBandToneAndDcGain) {
  const auto y = resample_poly(tone(1.0, 0.5f, 1000.0, 48000), 1, 3);
  float peak = 0.0f;
  for (std::size_t i = 2000; i < 14000; ++i) peak = std::max(peak, std::fabs(y[i]));
  EXPECT_NEAR(peak, 0.5f, 0.01f);
  const auto dc = resample_poly(std::vector<float>(8000, 0.25f), 2, 1);
  EXPECT_NEAR(dc[8000], 0.25f, 1e-3f);
}

TEST(Resample, StreamingMatchesBatchExactly) {
  for (auto [up, down] : {std::pair{1, 3}, std::pair{160, 441}, std::pair{2, 1}}) {
    const auto x = cat({tone(0.7, 0.4f, 300.0, 48000), silence(0.3), tone(0.5, 0.2f, 3000.0, 48000)});
    const auto batch = resample_poly(x, up, down);
    StreamResampler stream(up, down);
    std::vector<float> streamed;
    for (std::size_t offset = 0; offset < x.size(); offset += 4410) {
      stream.push(x.data() + offset, std::min<std::size_t>(4410, x.size() - offset), streamed);
    }
    ASSERT_GT(streamed.size(), batch.size() * 9 / 10) << up << "/" << down;
    ASSERT_LE(streamed.size(), batch.size());
    for (std::size_t i = 0; i < streamed.size(); ++i) ASSERT_EQ(streamed[i], batch[i]) << up << "/" << down << " at " << i;
  }
}

// ---- WAV -------------------------------------------------------------------------------------------
TEST(Wav, Pcm16RoundTripAndPadding) {
  const fs::path dir = temp_dir();
  const auto x = cat({tone(0.5), std::vector<float>{1.5f, -1.5f, 0.99999f, -0.5f}});
  write_pcm16_wav(dir / "a.wav", x.data(), x.size());
  EXPECT_EQ(fs::file_size(dir / "a.wav"), 44 + 2 * x.size());
  const auto y = load_wav_16k_mono(dir / "a.wav");
  ASSERT_EQ(y.size(), 16000u);  // padded to 1 s
  for (std::size_t i = 0; i < x.size(); ++i) {
    const float clipped = std::clamp(x[i], -1.0f, 1.0f);
    const float expected = static_cast<float>(static_cast<std::int16_t>(clipped * 32767.0f)) / 32768.0f;
    ASSERT_EQ(y[i], expected) << i;
  }
  EXPECT_EQ(y[x.size()], 0.0f);
  fs::remove_all(dir);
}

TEST(Wav, StereoFloatAndResampledInputs) {
  const fs::path dir = temp_dir();
  std::vector<float> stereo;
  for (int i = 0; i < 32000; ++i) {
    stereo.push_back(0.5f);
    stereo.push_back(i % 2 ? 2.0f : -0.25f);  // 2.0 is clipped to 1.0 before the channel mean
  }
  write_float_wav(dir / "s.wav", stereo.data(), stereo.size(), 16000, 2);
  const auto y = load_wav_16k_mono(dir / "s.wav");
  ASSERT_EQ(y.size(), 32000u);
  EXPECT_EQ(y[0], (0.5f + -0.25f) / 2.0f);
  EXPECT_EQ(y[1], (0.5f + 1.0f) / 2.0f);
  const auto x8 = tone(2.0, 0.3f, 200.0, 8000);
  write_pcm16_wav(dir / "e.wav", x8.data(), x8.size(), 8000);
  EXPECT_EQ(load_wav_16k_mono(dir / "e.wav").size(), 32000u);
  const auto segment = load_wav_segment_16k_mono(dir / "e.wav", 0.5, 1.75);
  EXPECT_EQ(segment.size(), 20000u);
  EXPECT_THROW(load_wav_segment_16k_mono(dir / "e.wav", 3.0, 4.0), std::runtime_error);
  EXPECT_THROW(load_wav_16k_mono(dir / "missing.wav"), std::runtime_error);
  fs::remove_all(dir);
}

// ---- ring buffer -----------------------------------------------------------------------------------
TEST(Ring, LosslessFanOutDeliversEverySampleToEveryConsumer) {
  AudioRing ring(3200, /*lossy=*/false);
  const int a = ring.add_consumer("context");
  const int b = ring.add_consumer("voice");
  std::vector<float> input(50000);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i);
  std::vector<float> got_a;
  std::vector<float> got_b;
  auto reader = [&](int consumer, std::vector<float>& sink, int delay_us) {
    AudioBlock block;
    std::int64_t expected = 0;
    while (ring.read(consumer, block)) {
      EXPECT_EQ(block.start_sample, expected);
      expected += static_cast<std::int64_t>(block.samples.size());
      sink.insert(sink.end(), block.samples.begin(), block.samples.end());
      if (delay_us) std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }
  };
  std::thread ta(reader, a, std::ref(got_a), 0);
  std::thread tb(reader, b, std::ref(got_b), 200);
  for (std::size_t offset = 0; offset < input.size(); offset += 1600) {
    ring.push(input.data() + offset, std::min<std::size_t>(1600, input.size() - offset));
  }
  ring.close();
  ta.join();
  tb.join();
  EXPECT_EQ(got_a, input);
  EXPECT_EQ(got_b, input);
}

TEST(Ring, LossyDropsOldestWholeBlocksOfALaggingConsumer) {
  AudioRing ring(16000, /*lossy=*/true);
  const int slow = ring.add_consumer("slow");
  std::vector<float> block(1600, 1.0f);
  for (int i = 0; i < 25; ++i) ring.push(block.data(), block.size());  // 40000 samples, never read
  EXPECT_EQ(ring.dropped(slow), 24000u);
  AudioBlock out;
  ASSERT_TRUE(ring.read(slow, out));
  EXPECT_EQ(out.start_sample, 24000);
  EXPECT_EQ(out.samples.size(), 1600u);
  ring.close();
  int blocks = 1;
  while (ring.read(slow, out)) ++blocks;
  EXPECT_EQ(blocks, 10);
}

TEST(Ring, DetachedConsumerNeverBlocksTheProducerAndTimeoutReturnsEmpty) {
  AudioRing ring(3200, /*lossy=*/false);
  const int dead = ring.add_consumer("dead");
  ring.detach(dead);
  std::vector<float> block(1600, 0.5f);
  for (int i = 0; i < 10; ++i) ring.push(block.data(), block.size());
  const int late = ring.add_consumer("late");
  AudioBlock out;
  ASSERT_TRUE(ring.read(late, out, 0, 20));
  EXPECT_TRUE(out.samples.empty());
  ring.close();
  EXPECT_FALSE(ring.read(late, out));
}

// ---- push-to-talk ----------------------------------------------------------------------------------
TEST(Ptt, ParsesCommands) {
  EXPECT_EQ(parse_ptt_command(""), PttCommand::toggle);
  EXPECT_EQ(parse_ptt_command("press\r"), PttCommand::toggle);
  EXPECT_EQ(parse_ptt_command(" START "), PttCommand::start);
  EXPECT_EQ(parse_ptt_command("stop"), PttCommand::stop);
  EXPECT_EQ(parse_ptt_command("cancel"), PttCommand::cancel);
  EXPECT_EQ(parse_ptt_command("Abort"), PttCommand::cancel);
  EXPECT_FALSE(parse_ptt_command("reboot").has_value());
  PttControl control;
  control.push(PttCommand::start, "5f0e0000-0000-4000-8000-0000000000a0");
  control.press();
  const auto first = control.poll_request();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->command, PttCommand::start);
  EXPECT_EQ(first->trace_id, "5f0e0000-0000-4000-8000-0000000000a0");
  EXPECT_EQ(control.poll(), PttCommand::toggle);
  EXPECT_FALSE(control.poll_request().has_value());
}

TEST(Ptt, FifoTriggerDeliversCommandsAndSurvivesWriterClose) {
  const fs::path dir = temp_dir();
  const fs::path fifo = dir / "ptt";
  PttControl control;
  {
    FifoTrigger trigger(fifo, control);
    struct stat info {};
    ASSERT_EQ(::stat(fifo.c_str(), &info), 0);
    EXPECT_TRUE(S_ISFIFO(info.st_mode));
    for (const char* text : {"press\nstart\n", "stop\nnonsense\n\n"}) {  // two writers, each closes
      const int fd = ::open(fifo.c_str(), O_WRONLY);
      ASSERT_GE(fd, 0);
      ASSERT_EQ(::write(fd, text, std::strlen(text)), static_cast<ssize_t>(std::strlen(text)));
      ::close(fd);
    }
    std::vector<PttCommand> got;
    for (int i = 0; i < 100 && got.size() < 4; ++i) {
      while (auto command = control.poll()) got.push_back(*command);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(got, (std::vector<PttCommand>{PttCommand::toggle, PttCommand::start, PttCommand::stop, PttCommand::toggle}));
  }
  std::ofstream(dir / "plain") << "x";
  EXPECT_THROW(FifoTrigger(dir / "plain", control), std::runtime_error);
  fs::remove_all(dir);
}
