#include <Arduino.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIC_ADC_PIN 5
#define REF_MIC_ADC_PIN 4
#define STATUS_LED 48
#define ENCODER_A_PIN 38
#define ENCODER_B_PIN 39
#define ENCODER_SW_PIN 40

#define I2S_BCLK 12
#define I2S_LRC 13
#define I2S_DOUT 14

#define SAMPLE_RATE 16000
#define BUFFER_SAMPLES 256
#define AUDIO_CONTROL_DIV 3
#define SERIAL_PERIOD_MS 250
#define PLOT_MODE 1
#define PLOT_PERIOD_MS 80
#define OVERNIGHT_LOG_MODE 1
#define OVERNIGHT_LOG_PERIOD_MS 60000UL
#define TIMING_LOG_PERIOD_MS 2000UL
#define AUDIO_ACTIVE_GAIN_THRESHOLD 0.05f
#define ANALYSIS_INTERVAL_MS 64

#define FFT_MIN_HZ 80.0f
#define FFT_MAX_HZ 5000.0f
#define FFT_BIN_SMOOTH_ALPHA 0.22f
#define BAND_ANALYSIS_SMOOTH_ALPHA 0.14f

#define HOLD_TIME_MS 4000
#define CLEAN_QUIET_RELEASE_ENABLED 1
#define CLEAN_QUIET_RELEASE_MS 850
#define CLEAN_QUIET_P2P_RATIO 0.32f
#define GAIN_ATTACK_MS 280.0f
#define GAIN_RELEASE_MS 1800.0f
#define MIX_ATTACK_MS 320.0f
#define MIX_RELEASE_MS 2000.0f
#define BAND_RISE_MS 2400.0f
#define BAND_FALL_MS 2400.0f

#define STARTUP_CALIBRATION_MS 15000UL
#define STARTUP_BASELINE_AVG_WEIGHT 0.60f
#define STARTUP_BASELINE_MAX_WEIGHT 0.40f

#define BASELINE_ALPHA 0.015f
#define TRIGGER_RATIO 1.22f
#define TRIGGER_FLOOR 28.0f
#define MIN_TRIGGER_P2P 80
#define TRIGGER_SCORE_ALPHA 0.09f
#define TRIGGER_ON_LEVEL 1.05f
#define TRIGGER_OFF_LEVEL 0.45f
#define MIC_SENSITIVITY 2.4f

#define ACTIVE_MASKING_GAIN 0.88f
#define ACTIVE_ADAPTIVE_MIX 1.00f
#define MIN_BAND_TARGET 0.00f
#define BAND_TARGET_FOLLOW_ALPHA 0.035f

#define ADAPTIVE_SOURCE_PINK_GAIN 0.30f
#define ADAPTIVE_SOURCE_WHITE_GAIN 0.70f
#define ADAPTIVE_EQ_GAIN 1.08f
#define OUTPUT_LOWPASS_HZ 2600.0f
#define OUTPUT_TONE_MIX 0.78f
#define OUTPUT_LEVEL_BOOST 3.60f
#define OUTPUT_SOFT_CLIP_DRIVE 1.25f
#define FEEDBACK_MATCH_HZ 220.0f
#define FEEDBACK_MAX_P2P_SUBTRACT 360.0f
#define FEEDBACK_BAND_SUPPRESS 0.55f
#define FEEDBACK_MIN_ACTIVITY 0.15f
#define ECHO_REF_BUFFER_SAMPLES 4096
#define ECHO_DELAY_MIN_SAMPLES 32
#define ECHO_DELAY_MAX_SAMPLES 160
#define ECHO_DELAY_STEP_SAMPLES 16
#define ECHO_GAIN_SMOOTH_ALPHA 0.18f
#define ECHO_SUBTRACT_MIX 0.92f
#define ECHO_MAX_GAIN 1.6f
#define ECHO_MIN_REF_POWER 0.0010f
#define ECHO_MIN_CORRELATION 0.12f
#define DUAL_MIC_REFERENCE_ENABLED 1
#define DUAL_MIC_SUBTRACT_MIX 0.82f
#define DUAL_MIC_MAX_GAIN 1.8f
#define DUAL_MIC_MIN_REF_RMS 0.004f
#define DUAL_MIC_MIN_CORRELATION 0.18f
#define DUAL_MIC_FEEDBACK_CORRELATION 0.62f
#define DUAL_MIC_FEEDBACK_REF_RATIO 1.05f
#define SELF_FEEDBACK_MIN_ACTIVITY 0.08f
#define SELF_FEEDBACK_TRIGGER_MATCH 0.68f
#define SELF_FEEDBACK_P2P_RATIO 0.45f
#define SELF_FEEDBACK_P2P_CLAMP_RATIO 0.12f
#define SELF_FEEDBACK_RELEASE_FRAMES 4
#define SELF_FEEDBACK_GUARD_ENABLED 1
#define SELF_FEEDBACK_FREQ_MATCH_ENABLED 0

#define VOLUME_STEP_COUNT 20
#define DEFAULT_VOLUME_STEP 20
#define ENCODER_COUNTS_PER_DETENT 4
#define BUTTON_DEBOUNCE_MS 35

constexpr int kBandCount = 6;

#if (ECHO_REF_BUFFER_SAMPLES & (ECHO_REF_BUFFER_SAMPLES - 1)) != 0
#error "ECHO_REF_BUFFER_SAMPLES must be a power of two"
#endif

struct BandDef {
  float minHz;
  float maxHz;
  float centerHz;
  float q;
};

constexpr BandDef kBands[kBandCount] = {
  {80.0f, 180.0f, 130.0f, 0.85f},
  {180.0f, 360.0f, 260.0f, 0.85f},
  {360.0f, 720.0f, 520.0f, 0.90f},
  {720.0f, 1400.0f, 1020.0f, 0.95f},
  {1400.0f, 2800.0f, 2050.0f, 0.95f},
  {2800.0f, 5000.0f, 3800.0f, 0.90f},
};

enum MaskState : uint8_t {
  STATE_LISTENING = 0,
  STATE_MASKING = 1,
  STATE_RELEASING = 2,
};

struct AnalysisFrame {
  int16_t samples[BUFFER_SAMPLES];
  int16_t refSamples[BUFFER_SAMPLES];
  int minVal;
  int maxVal;
  int peakToPeak;
  float rms;
  int refPeakToPeak;
  float refRms;
  float refCorrelation;
  float refToMicRatio;
  float dominantFreqHz;
  float dominantPower;
  float bandPowers[kBandCount];
};

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  void setBandPass(float centerHz, float q, float sampleRate, bool resetState = true) {
    const float omega = 2.0f * PI * centerHz / sampleRate;
    const float sinOmega = sinf(omega);
    const float cosOmega = cosf(omega);
    const float alpha = sinOmega / (2.0f * q);

    const float rawB0 = alpha;
    const float rawB1 = 0.0f;
    const float rawB2 = -alpha;
    const float rawA0 = 1.0f + alpha;
    const float rawA1 = -2.0f * cosOmega;
    const float rawA2 = 1.0f - alpha;

    b0 = rawB0 / rawA0;
    b1 = rawB1 / rawA0;
    b2 = rawB2 / rawA0;
    a1 = rawA1 / rawA0;
    a2 = rawA2 / rawA0;
    if (resetState) {
      z1 = 0.0f;
      z2 = 0.0f;
    }
  }

  void setLowPass(float cutoffHz, float q, float sampleRate, bool resetState = true) {
    const float omega = 2.0f * PI * cutoffHz / sampleRate;
    const float sinOmega = sinf(omega);
    const float cosOmega = cosf(omega);
    const float alpha = sinOmega / (2.0f * q);

    const float rawB0 = (1.0f - cosOmega) * 0.5f;
    const float rawB1 = 1.0f - cosOmega;
    const float rawB2 = (1.0f - cosOmega) * 0.5f;
    const float rawA0 = 1.0f + alpha;
    const float rawA1 = -2.0f * cosOmega;
    const float rawA2 = 1.0f - alpha;

    b0 = rawB0 / rawA0;
    b1 = rawB1 / rawA0;
    b2 = rawB2 / rawA0;
    a1 = rawA1 / rawA0;
    a2 = rawA2 / rawA0;
    if (resetState) {
      z1 = 0.0f;
      z2 = 0.0f;
    }
  }

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

struct PinkNoiseState {
  float b0 = 0.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float b3 = 0.0f;
  float b4 = 0.0f;
  float b5 = 0.0f;
  float b6 = 0.0f;
};

struct NoiseColorMix {
  float white = 0.0f;
  float pink = 0.0f;
  float brown = 0.0f;
};

struct SharedState {
  MaskState maskState;
  float baselineP2P;
  float currentGain;
  float targetGain;
  float currentAdaptiveMix;
  float targetAdaptiveMix;
  float volumeScale;
  float lastDominantFreqHz;
  float outputFocusHz;
  float bandTargets[kBandCount];
  int lastPeakToPeak;
  float lastRms;
  int lastRefPeakToPeak;
  float lastRefCorrelation;
  float lastRefToMicRatio;
  float measuredSampleRateHz;
  float measuredFrameMs;
  float lastDetectToStateMs;
  float lastStateToAudioMs;
  float lastDetectToAudioMs;
  bool muted;
};

int16_t speakerBuffer[BUFFER_SAMPLES * 2];
float gAnalysisWindow[BUFFER_SAMPLES];
PinkNoiseState gPinkState;
float gWhiteFast = 0.0f;
float gWhiteSlow = 0.0f;
float gBrownState = 0.0f;
float gEchoReference[ECHO_REF_BUFFER_SAMPLES] = {0.0f};
volatile uint32_t gEchoWriteIndex = 0;
Biquad gToneFilter;
uint32_t gBootId = 0;

SharedState gState = {
  STATE_LISTENING,
  300.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  (float)DEFAULT_VOLUME_STEP / (float)VOLUME_STEP_COUNT,
  500.0f,
  0.0f,
  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
  0,
  0.0f,
  0,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  -1.0f,
  -1.0f,
  -1.0f,
  false,
};

portMUX_TYPE gStateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gEncoderMux = portMUX_INITIALIZER_UNLOCKED;
volatile int32_t gEncoderTransitionDelta = 0;
volatile uint8_t gEncoderPrevState = 0;
volatile uint32_t gLastDetectionUs = 0;
volatile uint32_t gLastStateMaskingUs = 0;
volatile uint32_t gPendingDetectionUs = 0;
volatile uint32_t gPendingStateMaskingUs = 0;

SharedState snapshotState() {
  SharedState copy;
  portENTER_CRITICAL(&gStateMux);
  copy = gState;
  portEXIT_CRITICAL(&gStateMux);
  return copy;
}

void publishAudioState(float currentGain, float currentAdaptiveMix, float outputFocusHz) {
  portENTER_CRITICAL(&gStateMux);
  gState.currentGain = currentGain;
  gState.currentAdaptiveMix = currentAdaptiveMix;
  gState.outputFocusHz = outputFocusHz;
  portEXIT_CRITICAL(&gStateMux);
}

void publishCaptureTiming(float sampleRateHz, float frameMs) {
  portENTER_CRITICAL(&gStateMux);
  gState.measuredSampleRateHz = sampleRateHz;
  gState.measuredFrameMs = frameMs;
  portEXIT_CRITICAL(&gStateMux);
}

void publishLatencyTiming(float detectToStateMs, float stateToAudioMs, float detectToAudioMs) {
  portENTER_CRITICAL(&gStateMux);
  gState.lastDetectToStateMs = detectToStateMs;
  gState.lastStateToAudioMs = stateToAudioMs;
  gState.lastDetectToAudioMs = detectToAudioMs;
  portEXIT_CRITICAL(&gStateMux);
}

void publishAnalysisState(
  MaskState maskState,
  float baselineP2P,
  float targetGain,
  float targetAdaptiveMix,
  float volumeScale,
  float dominantFreqHz,
  const float *bandTargets,
  int peakToPeak,
  float rms,
  int refPeakToPeak,
  float refCorrelation,
  float refToMicRatio,
  bool muted
) {
  portENTER_CRITICAL(&gStateMux);
  gState.maskState = maskState;
  gState.baselineP2P = baselineP2P;
  gState.targetGain = targetGain;
  gState.targetAdaptiveMix = targetAdaptiveMix;
  gState.volumeScale = volumeScale;
  gState.lastDominantFreqHz = dominantFreqHz;
  for (int i = 0; i < kBandCount; ++i) {
    gState.bandTargets[i] = bandTargets[i];
  }
  gState.lastPeakToPeak = peakToPeak;
  gState.lastRms = rms;
  gState.lastRefPeakToPeak = refPeakToPeak;
  gState.lastRefCorrelation = refCorrelation;
  gState.lastRefToMicRatio = refToMicRatio;
  gState.muted = muted;
  portEXIT_CRITICAL(&gStateMux);
}

float volumeScaleFromStep(int step) {
  const int clamped = constrain(step, 0, VOLUME_STEP_COUNT);
  return (float)clamped / (float)VOLUME_STEP_COUNT;
}

IRAM_ATTR void encoderISR() {
  static const int8_t kTransitionTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0
  };

  const uint8_t a = (uint8_t)gpio_get_level((gpio_num_t)ENCODER_A_PIN);
  const uint8_t b = (uint8_t)gpio_get_level((gpio_num_t)ENCODER_B_PIN);
  const uint8_t currentState = (uint8_t)((a << 1) | b);
  const uint8_t transition = (uint8_t)((gEncoderPrevState << 2) | currentState);

  portENTER_CRITICAL_ISR(&gEncoderMux);
  gEncoderTransitionDelta += kTransitionTable[transition];
  gEncoderPrevState = currentState;
  portEXIT_CRITICAL_ISR(&gEncoderMux);
}

void setupEncoder() {
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);

  const uint8_t a = (uint8_t)gpio_get_level((gpio_num_t)ENCODER_A_PIN);
  const uint8_t b = (uint8_t)gpio_get_level((gpio_num_t)ENCODER_B_PIN);
  gEncoderPrevState = (uint8_t)((a << 1) | b);

  attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B_PIN), encoderISR, CHANGE);
}

void setupI2S() {
  i2s_config_t config;
  memset(&config, 0, sizeof(config));
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 12;
  config.dma_buf_len = BUFFER_SAMPLES;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_LRC;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

void initializeAnalysisWindow() {
  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    gAnalysisWindow[i] = 0.5f - 0.5f * cosf((2.0f * PI * i) / (BUFFER_SAMPLES - 1));
  }
}

float uniformNoise() {
  return ((float)esp_random() / (float)UINT32_MAX) * 2.0f - 1.0f;
}

float smoothWhiteNoise() {
  const float raw = uniformNoise();
  gWhiteFast += (raw - gWhiteFast) * 0.16f;
  gWhiteSlow += (raw - gWhiteSlow) * 0.04f;
  return constrain(gWhiteFast * 0.72f + gWhiteSlow * 0.28f, -1.0f, 1.0f);
}

float pinkNoise() {
  const float white = uniformNoise();
  gPinkState.b0 = 0.99886f * gPinkState.b0 + white * 0.0555179f;
  gPinkState.b1 = 0.99332f * gPinkState.b1 + white * 0.0750759f;
  gPinkState.b2 = 0.96900f * gPinkState.b2 + white * 0.1538520f;
  gPinkState.b3 = 0.86650f * gPinkState.b3 + white * 0.3104856f;
  gPinkState.b4 = 0.55000f * gPinkState.b4 + white * 0.5329522f;
  gPinkState.b5 = -0.7616f * gPinkState.b5 - white * 0.0168980f;
  const float pink =
    gPinkState.b0 +
    gPinkState.b1 +
    gPinkState.b2 +
    gPinkState.b3 +
    gPinkState.b4 +
    gPinkState.b5 +
    gPinkState.b6 +
    white * 0.5362f;
  gPinkState.b6 = white * 0.115926f;
  return constrain(pink * 0.11f, -1.0f, 1.0f);
}

float brownNoise() {
  const float white = uniformNoise();
  gBrownState += white * 0.020f;
  gBrownState *= 0.995f;
  return constrain(gBrownState * 2.8f, -1.0f, 1.0f);
}

NoiseColorMix sourceMixFromHz(float hz) {
  const float lowBias = constrain((700.0f - hz) / 600.0f, 0.0f, 1.0f);
  const float highBias = constrain((hz - 1300.0f) / 1800.0f, 0.0f, 1.0f);

  NoiseColorMix mix;
  mix.brown = 0.10f + 0.70f * lowBias;
  mix.white = 0.10f + 0.70f * highBias;
  mix.pink = 1.00f;

  const float sum = mix.white + mix.pink + mix.brown;
  if (sum > 0.0001f) {
    mix.white /= sum;
    mix.pink /= sum;
    mix.brown /= sum;
  }
  return mix;
}

void pushEchoReference(float sample) {
  const uint32_t writeIndex = gEchoWriteIndex;
  gEchoReference[writeIndex & (ECHO_REF_BUFFER_SAMPLES - 1)] = sample;
  gEchoWriteIndex = writeIndex + 1;
}

float readEchoReference(uint32_t index) {
  return gEchoReference[index & (ECHO_REF_BUFFER_SAMPLES - 1)];
}

void applyAdaptiveEchoCancellation(AnalysisFrame &frame, uint32_t captureEndEchoIndex) {
  static int selectedDelaySamples = 80;
  static float smoothedEchoGain = 0.0f;

  float bestCorrelation = 0.0f;
  float bestGainEstimate = smoothedEchoGain;
  int bestDelay = selectedDelaySamples;

  for (int delay = ECHO_DELAY_MIN_SAMPLES; delay <= ECHO_DELAY_MAX_SAMPLES; delay += ECHO_DELAY_STEP_SAMPLES) {
    const uint32_t baseIndex = captureEndEchoIndex - BUFFER_SAMPLES - (uint32_t)delay;
    float corr = 0.0f;
    float refPower = 0.0f;
    float micPower = 0.0f;

    for (int i = 0; i < BUFFER_SAMPLES; ++i) {
      const float mic = (float)frame.samples[i] / 2048.0f;
      const float ref = readEchoReference(baseIndex + (uint32_t)i);
      corr += mic * ref;
      refPower += ref * ref;
      micPower += mic * mic;
    }

    if (refPower < ECHO_MIN_REF_POWER || micPower < 1e-6f) {
      continue;
    }

    const float corrNorm = fabsf(corr) / sqrtf(refPower * micPower + 1e-9f);
    if (corrNorm > bestCorrelation) {
      bestCorrelation = corrNorm;
      bestDelay = delay;
      bestGainEstimate = corr / (refPower + 1e-6f);
    }
  }

  if (bestCorrelation >= ECHO_MIN_CORRELATION) {
    selectedDelaySamples = bestDelay;
    smoothedEchoGain += (bestGainEstimate - smoothedEchoGain) * ECHO_GAIN_SMOOTH_ALPHA;
  } else {
    smoothedEchoGain *= 0.98f;
  }
  smoothedEchoGain = constrain(smoothedEchoGain, -ECHO_MAX_GAIN, ECHO_MAX_GAIN);

  const uint32_t subtractBaseIndex = captureEndEchoIndex - BUFFER_SAMPLES - (uint32_t)selectedDelaySamples;
  float sumSquares = 0.0f;
  int minSample = 32767;
  int maxSample = -32768;

  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    const float mic = (float)frame.samples[i] / 2048.0f;
    const float ref = readEchoReference(subtractBaseIndex + (uint32_t)i);
    const float cleaned = constrain(mic - smoothedEchoGain * ref * ECHO_SUBTRACT_MIX, -1.6f, 1.6f);
    const int16_t sample = (int16_t)constrain(cleaned * 2048.0f, -32767.0f, 32767.0f);
    frame.samples[i] = sample;
    minSample = min(minSample, (int)sample);
    maxSample = max(maxSample, (int)sample);
    sumSquares += cleaned * cleaned;
  }

  frame.minVal = minSample;
  frame.maxVal = maxSample;
  frame.peakToPeak = maxSample - minSample;
  frame.rms = sqrtf(sumSquares / BUFFER_SAMPLES);
}

void applyDualMicReferenceCancellation(AnalysisFrame &frame) {
#if DUAL_MIC_REFERENCE_ENABLED
  float corr = 0.0f;
  float micPower = 0.0f;
  float refPower = 0.0f;
  int micMin = 32767;
  int micMax = -32768;
  int refMin = 32767;
  int refMax = -32768;

  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    const float mic = (float)frame.samples[i] / 2048.0f;
    const float ref = (float)frame.refSamples[i] / 2048.0f;
    corr += mic * ref;
    micPower += mic * mic;
    refPower += ref * ref;
    micMin = min(micMin, (int)frame.samples[i]);
    micMax = max(micMax, (int)frame.samples[i]);
    refMin = min(refMin, (int)frame.refSamples[i]);
    refMax = max(refMax, (int)frame.refSamples[i]);
  }

  const float micRmsBefore = sqrtf(micPower / BUFFER_SAMPLES);
  const float refRms = sqrtf(refPower / BUFFER_SAMPLES);
  const float corrNorm = fabsf(corr) / sqrtf(micPower * refPower + 1e-9f);
  const float refToMicRatio = refRms / fmaxf(micRmsBefore, 1e-6f);

  frame.refPeakToPeak = refMax - refMin;
  frame.refRms = refRms;
  frame.refCorrelation = corrNorm;
  frame.refToMicRatio = refToMicRatio;

  const bool usableReference =
    (refRms >= DUAL_MIC_MIN_REF_RMS) &&
    (corrNorm >= DUAL_MIC_MIN_CORRELATION);

  if (!usableReference) {
    frame.minVal = micMin;
    frame.maxVal = micMax;
    frame.peakToPeak = micMax - micMin;
    frame.rms = micRmsBefore;
    return;
  }

  const float gain = constrain(corr / (refPower + 1e-6f), -DUAL_MIC_MAX_GAIN, DUAL_MIC_MAX_GAIN);
  float sumSquares = 0.0f;
  int cleanMin = 32767;
  int cleanMax = -32768;

  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    const float mic = (float)frame.samples[i] / 2048.0f;
    const float ref = (float)frame.refSamples[i] / 2048.0f;
    const float cleaned = constrain(mic - gain * ref * DUAL_MIC_SUBTRACT_MIX, -1.6f, 1.6f);
    const int16_t sample = (int16_t)constrain(cleaned * 2048.0f, -32767.0f, 32767.0f);
    frame.samples[i] = sample;
    cleanMin = min(cleanMin, (int)sample);
    cleanMax = max(cleanMax, (int)sample);
    sumSquares += cleaned * cleaned;
  }

  frame.minVal = cleanMin;
  frame.maxVal = cleanMax;
  frame.peakToPeak = cleanMax - cleanMin;
  frame.rms = sqrtf(sumSquares / BUFFER_SAMPLES);
#else
  (void)frame;
#endif
}

void runFFT(float *real, float *imag, int count) {
  int j = 0;
  for (int i = 0; i < count; ++i) {
    if (i < j) {
      const float tempReal = real[i];
      const float tempImag = imag[i];
      real[i] = real[j];
      imag[i] = imag[j];
      real[j] = tempReal;
      imag[j] = tempImag;
    }

    int bit = count >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
  }

  for (int len = 2; len <= count; len <<= 1) {
    const float angle = -2.0f * PI / len;
    const float wLenReal = cosf(angle);
    const float wLenImag = sinf(angle);

    for (int start = 0; start < count; start += len) {
      float wReal = 1.0f;
      float wImag = 0.0f;
      for (int offset = 0; offset < (len >> 1); ++offset) {
        const int evenIndex = start + offset;
        const int oddIndex = evenIndex + (len >> 1);

        const float oddReal = real[oddIndex] * wReal - imag[oddIndex] * wImag;
        const float oddImag = real[oddIndex] * wImag + imag[oddIndex] * wReal;

        real[oddIndex] = real[evenIndex] - oddReal;
        imag[oddIndex] = imag[evenIndex] - oddImag;
        real[evenIndex] += oddReal;
        imag[evenIndex] += oddImag;

        const float nextWReal = wReal * wLenReal - wImag * wLenImag;
        wImag = wReal * wLenImag + wImag * wLenReal;
        wReal = nextWReal;
      }
    }
  }
}

void analyzeSpectrum(AnalysisFrame &frame) {
  float real[BUFFER_SAMPLES];
  float imag[BUFFER_SAMPLES];
  static float smoothedMagnitude[BUFFER_SAMPLES / 2] = {0.0f};

  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    real[i] = ((float)frame.samples[i] / 2048.0f) * gAnalysisWindow[i];
    imag[i] = 0.0f;
  }

  runFFT(real, imag, BUFFER_SAMPLES);

  for (int band = 0; band < kBandCount; ++band) {
    frame.bandPowers[band] = 0.0f;
  }
  frame.dominantPower = 0.0f;
  frame.dominantFreqHz = snapshotState().lastDominantFreqHz;

  for (int bin = 1; bin < (BUFFER_SAMPLES / 2); ++bin) {
    const float binHz = ((float)bin * SAMPLE_RATE) / BUFFER_SAMPLES;
    if (binHz < FFT_MIN_HZ || binHz > FFT_MAX_HZ) {
      continue;
    }
    const float mag2 = real[bin] * real[bin] + imag[bin] * imag[bin];
    smoothedMagnitude[bin] += (mag2 - smoothedMagnitude[bin]) * FFT_BIN_SMOOTH_ALPHA;
  }

  for (int band = 0; band < kBandCount; ++band) {
    float bandPower = 0.0f;
    float weightedFreq = 0.0f;
    const int minBin = max(1, (int)floorf((kBands[band].minHz * BUFFER_SAMPLES) / SAMPLE_RATE));
    const int maxBin = min((BUFFER_SAMPLES / 2) - 1, (int)ceilf((kBands[band].maxHz * BUFFER_SAMPLES) / SAMPLE_RATE));

    for (int bin = minBin; bin <= maxBin; ++bin) {
      const float binPower = smoothedMagnitude[bin];
      const float binHz = ((float)bin * SAMPLE_RATE) / BUFFER_SAMPLES;
      bandPower += binPower;
      weightedFreq += binPower * binHz;
    }

    frame.bandPowers[band] = bandPower;
    if (bandPower > frame.dominantPower) {
      frame.dominantPower = bandPower;
      frame.dominantFreqHz = (bandPower > 0.0f) ? (weightedFreq / bandPower) : kBands[band].centerHz;
    }
  }
}

void captureAnalysisFrame(AnalysisFrame &frame) {
  uint32_t nextSampleUs = micros();
  const uint32_t captureStartUs = nextSampleUs;
  int32_t micSum = 0;
  int32_t refSum = 0;
  frame.refPeakToPeak = 0;
  frame.refRms = 0.0f;
  frame.refCorrelation = 0.0f;
  frame.refToMicRatio = 0.0f;

  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    while ((int32_t)(micros() - nextSampleUs) < 0) {
    }
    nextSampleUs += (1000000UL / SAMPLE_RATE);

    const int micRaw = analogRead(MIC_ADC_PIN);
    const int refRaw = analogRead(REF_MIC_ADC_PIN);
    frame.samples[i] = (int16_t)micRaw;
    frame.refSamples[i] = (int16_t)refRaw;
    micSum += micRaw;
    refSum += refRaw;
  }

  const uint32_t captureElapsedUs = nextSampleUs - captureStartUs;
  if (captureElapsedUs > 0) {
    publishCaptureTiming(
      ((float)BUFFER_SAMPLES * 1000000.0f) / (float)captureElapsedUs,
      (float)captureElapsedUs / 1000.0f
    );
  }

  const int16_t micMean = (int16_t)(micSum / BUFFER_SAMPLES);
  const int16_t refMean = (int16_t)(refSum / BUFFER_SAMPLES);
  for (int i = 0; i < BUFFER_SAMPLES; ++i) {
    frame.samples[i] = (int16_t)(frame.samples[i] - micMean);
    frame.refSamples[i] = (int16_t)(frame.refSamples[i] - refMean);
  }

  applyDualMicReferenceCancellation(frame);
  const uint32_t captureEndEchoIndex = gEchoWriteIndex;
  applyAdaptiveEchoCancellation(frame, captureEndEchoIndex);
  analyzeSpectrum(frame);
}

float updateRampedValue(float currentValue, float targetValue, float riseMs, float fallMs) {
  const float rampMs = (targetValue >= currentValue) ? riseMs : fallMs;
  if (rampMs <= 0.0f) {
    return targetValue;
  }

  const float step = 1.0f / ((rampMs / 1000.0f) * SAMPLE_RATE);
  currentValue += (targetValue - currentValue) * step;

  if (fabsf(targetValue - currentValue) < 0.0005f) {
    currentValue = targetValue;
  }

  return currentValue;
}

float softClip(float x, float drive) {
  const float normalizer = tanhf(drive);
  if (normalizer <= 0.0f) {
    return x;
  }
  return tanhf(x * drive) / normalizer;
}

void updateMaskingState(const AnalysisFrame &frame, SharedState &state, unsigned long nowMs, unsigned long &lastTriggerAt) {
  float adaptiveThreshold = state.baselineP2P * TRIGGER_RATIO + TRIGGER_FLOOR;
  const int threshold = (int)fmaxf((float)MIN_TRIGGER_P2P, adaptiveThreshold);
  const float triggerMetric = (float)frame.peakToPeak / fmaxf(1.0f, (float)threshold);
  const bool rawFrameTriggered = frame.peakToPeak > threshold;
  const MaskState previousMaskState = state.maskState;
  static float smoothedTriggerScore = 0.0f;
  smoothedTriggerScore += (triggerMetric - smoothedTriggerScore) * TRIGGER_SCORE_ALPHA;

  const bool triggerOn = smoothedTriggerScore >= TRIGGER_ON_LEVEL;
  const bool triggerOff = smoothedTriggerScore <= TRIGGER_OFF_LEVEL;

  if (!triggerOn && state.maskState == STATE_LISTENING) {
    state.baselineP2P = state.baselineP2P * (1.0f - BASELINE_ALPHA) + frame.peakToPeak * BASELINE_ALPHA;
  }

  if (triggerOn) {
    lastTriggerAt = nowMs;
    state.maskState = STATE_MASKING;
  } else if (triggerOff && (nowMs - lastTriggerAt) >= HOLD_TIME_MS) {
    state.maskState = STATE_LISTENING;
  }

  if (state.maskState == STATE_MASKING) {
    state.targetGain = ACTIVE_MASKING_GAIN;
    state.targetAdaptiveMix = ACTIVE_ADAPTIVE_MIX;
  } else {
    state.targetGain = 0.0f;
    state.targetAdaptiveMix = 0.0f;
  }

  if (rawFrameTriggered) {
    const uint32_t nowUs = micros();
    gLastDetectionUs = nowUs;
    if (previousMaskState != STATE_MASKING) {
      gPendingDetectionUs = nowUs;
    }
  }
  if (previousMaskState != STATE_MASKING && state.maskState == STATE_MASKING) {
    const uint32_t nowUs = micros();
    gLastStateMaskingUs = nowUs;
    gPendingStateMaskingUs = nowUs;
    const uint32_t detectUs = gPendingDetectionUs ? gPendingDetectionUs : nowUs;
    publishLatencyTiming((float)(nowUs - detectUs) / 1000.0f, -1.0f, -1.0f);
  }

  state.lastPeakToPeak = frame.peakToPeak;
  state.lastRms = frame.rms;
}

void printStatus(const SharedState &state) {
  const unsigned long now = millis();
  static unsigned long lastSerialAt = 0;
  static unsigned long lastTimingAt = 0;
  if ((now - lastTimingAt) >= TIMING_LOG_PERIOD_MS) {
    lastTimingAt = now;
    Serial.printf(
      "timing sampleRate=%.0fHz frame=%.2fms detectToState=%.1fms stateToAudio=%.1fms detectToAudio=%.1fms\n",
      state.measuredSampleRateHz,
      state.measuredFrameMs,
      state.lastDetectToStateMs,
      state.lastStateToAudioMs,
      state.lastDetectToAudioMs
    );
  }

  const unsigned long periodMs =
    OVERNIGHT_LOG_MODE ? OVERNIGHT_LOG_PERIOD_MS :
    (PLOT_MODE ? PLOT_PERIOD_MS : SERIAL_PERIOD_MS);
  if ((now - lastSerialAt) < periodMs) {
    return;
  }

  lastSerialAt = now;
#if OVERNIGHT_LOG_MODE
  const float threshold = fmaxf((float)MIN_TRIGGER_P2P, state.baselineP2P * TRIGGER_RATIO + TRIGGER_FLOOR);
  Serial.printf(
    "overnight,%lu,%lu,%u,%u,%d,%.1f,%.1f,%.0f,%.0f,%.3f,%.3f,%.2f,%u,%u,%d,%.2f,%.2f,%.4f,%.0f,%.2f,%.1f,%.1f,%.1f\n",
    (unsigned long)gBootId,
    (unsigned long)now,
    (unsigned)ESP.getFreeHeap(),
    (unsigned)state.maskState,
    state.lastPeakToPeak,
    state.baselineP2P,
    threshold,
    state.lastDominantFreqHz,
    state.outputFocusHz,
    state.currentGain,
    state.currentAdaptiveMix,
    state.volumeScale,
    state.muted ? 1U : 0U,
    (state.maskState == STATE_MASKING) ? 1U : 0U,
    state.lastRefPeakToPeak,
    state.lastRefCorrelation,
    state.lastRefToMicRatio,
    state.lastRms,
    state.measuredSampleRateHz,
    state.measuredFrameMs,
    state.lastDetectToStateMs,
    state.lastStateToAudioMs,
    state.lastDetectToAudioMs
  );
#elif PLOT_MODE
  const float threshold = fmaxf((float)MIN_TRIGGER_P2P, state.baselineP2P * TRIGGER_RATIO + TRIGGER_FLOOR);
  const unsigned maskActive = (state.maskState == STATE_MASKING) ? 1U : 0U;
  const unsigned muted = state.muted ? 1U : 0U;
  Serial.printf(
    "p2p:%d,baseline:%.1f,threshold:%.1f,inHz:%.0f,outHz:%.0f,b0:%.2f,b1:%.2f,b2:%.2f,b3:%.2f,b4:%.2f,b5:%.2f,gain:%.3f,mix:%.3f,mask:%u,mute:%u,refP2P:%d,refCorr:%.2f,refRatio:%.2f\n",
    state.lastPeakToPeak,
    state.baselineP2P,
    threshold,
    state.lastDominantFreqHz,
    state.outputFocusHz,
    state.bandTargets[0],
    state.bandTargets[1],
    state.bandTargets[2],
    state.bandTargets[3],
    state.bandTargets[4],
    state.bandTargets[5],
    state.currentGain,
    state.currentAdaptiveMix,
    maskActive,
    muted,
    state.lastRefPeakToPeak,
    state.lastRefCorrelation,
    state.lastRefToMicRatio
  );
#else
  Serial.printf(
    "p2p=%d baseline=%.1f state=%u gain=%.3f mix=%.3f vol=%.0f%% mute=%u inHz=%.0f outBand=%.0f bands=[%.2f %.2f %.2f %.2f %.2f %.2f] rms=%.4f\n",
    state.lastPeakToPeak,
    state.baselineP2P,
    (unsigned)state.maskState,
    state.currentGain,
    state.currentAdaptiveMix,
    state.volumeScale * 100.0f,
    state.muted ? 1U : 0U,
    state.lastDominantFreqHz,
    state.outputFocusHz,
    state.bandTargets[0],
    state.bandTargets[1],
    state.bandTargets[2],
    state.bandTargets[3],
    state.bandTargets[4],
    state.bandTargets[5],
    state.lastRms
  );
#endif
}

void audioTask(void *parameter) {
  (void)parameter;
  Serial.printf("audioTask core=%d\n", xPortGetCoreID());

  Biquad bandFilters[kBandCount];
  for (int band = 0; band < kBandCount; ++band) {
    bandFilters[band].setBandPass(kBands[band].centerHz, kBands[band].q, SAMPLE_RATE, true);
  }

  float currentGain = 0.0f;
  float currentAdaptiveMix = 0.0f;
  float smoothedOutputVolume = snapshotState().volumeScale;
  float smoothedBandWeights[kBandCount] = {
    MIN_BAND_TARGET, MIN_BAND_TARGET, MIN_BAND_TARGET,
    MIN_BAND_TARGET, MIN_BAND_TARGET, MIN_BAND_TARGET
  };
  float interpPrevAdaptiveLayer = 0.0f;
  float interpNextAdaptiveLayer = 0.0f;
  int interpPhase = AUDIO_CONTROL_DIV;
  float lastOutputFocusHz = 0.0f;

  for (;;) {
    const SharedState state = snapshotState();

    for (int i = 0; i < BUFFER_SAMPLES; ++i) {
      currentGain = updateRampedValue(currentGain, state.targetGain, GAIN_ATTACK_MS, GAIN_RELEASE_MS);
      currentAdaptiveMix = updateRampedValue(currentAdaptiveMix, state.targetAdaptiveMix, MIX_ATTACK_MS, MIX_RELEASE_MS);
      if (currentGain >= AUDIO_ACTIVE_GAIN_THRESHOLD && gPendingStateMaskingUs != 0) {
        const uint32_t nowUs = micros();
        const uint32_t detectUs = gPendingDetectionUs ? gPendingDetectionUs : gPendingStateMaskingUs;
        publishLatencyTiming(
          (float)(gPendingStateMaskingUs - detectUs) / 1000.0f,
          (float)(nowUs - gPendingStateMaskingUs) / 1000.0f,
          (float)(nowUs - detectUs) / 1000.0f
        );
        gPendingStateMaskingUs = 0;
        gPendingDetectionUs = 0;
      }
      const float targetOutputVolume = state.muted ? 0.0f : state.volumeScale;
      smoothedOutputVolume = updateRampedValue(smoothedOutputVolume, targetOutputVolume, 180.0f, 180.0f);
      for (int band = 0; band < kBandCount; ++band) {
        smoothedBandWeights[band] = updateRampedValue(
          smoothedBandWeights[band],
          state.bandTargets[band],
          BAND_RISE_MS,
          BAND_FALL_MS
        );
      }

      if (interpPhase >= AUDIO_CONTROL_DIV) {
        float bandWeights[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float weightSum = 0.0f;
        float weightedCenterHz = 0.0f;
        for (int band = 0; band < kBandCount; ++band) {
          bandWeights[band] = fmaxf(0.0f, smoothedBandWeights[band] - MIN_BAND_TARGET);
          weightSum += bandWeights[band];
          weightedCenterHz += bandWeights[band] * kBands[band].centerHz;
        }

        const float focusHzForSource = (weightSum > 0.001f)
          ? (weightedCenterHz / weightSum)
          : state.lastDominantFreqHz;
        const NoiseColorMix sourceMix = sourceMixFromHz(focusHzForSource);

        // Pure adaptive source: no permanent background bed is mixed in.
        const float shapedSource =
          smoothWhiteNoise() * sourceMix.white * ADAPTIVE_SOURCE_WHITE_GAIN +
          pinkNoise() * sourceMix.pink * ADAPTIVE_SOURCE_PINK_GAIN +
          brownNoise() * sourceMix.brown;

        float shapedSum = 0.0f;
        for (int band = 0; band < kBandCount; ++band) {
          const float bandSignal = bandFilters[band].process(shapedSource);
          shapedSum += bandSignal * bandWeights[band];
        }

        const float newAdaptiveLayer = (weightSum > 0.001f)
          ? (shapedSum / sqrtf(weightSum))
          : 0.0f;

        interpPrevAdaptiveLayer = interpNextAdaptiveLayer;
        interpNextAdaptiveLayer = newAdaptiveLayer;
        interpPhase = 0;
        lastOutputFocusHz = (weightSum > 0.001f) ? (weightedCenterHz / weightSum) : 0.0f;
      }

      const float interpT = (float)interpPhase / (float)AUDIO_CONTROL_DIV;
      const float adaptiveLayer =
        interpPrevAdaptiveLayer + (interpNextAdaptiveLayer - interpPrevAdaptiveLayer) * interpT;
      ++interpPhase;

      const float mixedAdaptiveNoise = adaptiveLayer * ADAPTIVE_EQ_GAIN * currentAdaptiveMix;
      const float softenedNoise = gToneFilter.process(mixedAdaptiveNoise);
      const float eqNoise = mixedAdaptiveNoise * (1.0f - OUTPUT_TONE_MIX) + softenedNoise * OUTPUT_TONE_MIX;
      const float leveledNoise = softClip(eqNoise * currentGain * smoothedOutputVolume * OUTPUT_LEVEL_BOOST, OUTPUT_SOFT_CLIP_DRIVE);
      const float scaled = constrain(leveledNoise * 32767.0f, -32767.0f, 32767.0f);
      const int16_t pcm = (int16_t)scaled;

      pushEchoReference(leveledNoise);
      speakerBuffer[2 * i] = pcm;
      speakerBuffer[2 * i + 1] = pcm;

      if (i == (BUFFER_SAMPLES - 1)) {
        publishAudioState(currentGain, currentAdaptiveMix, lastOutputFocusHz);
      }
    }

    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, speakerBuffer, sizeof(speakerBuffer), &bytesWritten, portMAX_DELAY);
  }
}

void analysisTask(void *parameter) {
  (void)parameter;
  Serial.printf("analysisTask core=%d\n", xPortGetCoreID());

  unsigned long ledOffAt = 0;
  unsigned long lastTriggerAt = 0;
  unsigned long cleanQuietSince = 0;
  unsigned long lastButtonChangeAt = 0;
  bool lastButtonReading = true;
  bool debouncedButtonState = true;
  int volumeStep = DEFAULT_VOLUME_STEP;
  int encoderCountRemainder = 0;

  bool smoothingInitialized = false;
  float smoothedPeakToPeak = 0.0f;
  float smoothedRms = 0.0f;
  float smoothedDominantFreqHz = 500.0f;
  float smoothedBandPower[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float heldBandTargets[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t selfFeedbackFrames = 0;
  bool startupCalibrated = false;
  unsigned long startupCalibrationStartedAt = 0;
  uint32_t startupCalibrationSamples = 0;
  float startupCalibrationSumP2P = 0.0f;
  float startupCalibrationMaxP2P = 0.0f;

  for (;;) {
    const unsigned long startedAt = millis();

    int32_t transitionDelta = 0;
    portENTER_CRITICAL(&gEncoderMux);
    transitionDelta = gEncoderTransitionDelta;
    gEncoderTransitionDelta = 0;
    portEXIT_CRITICAL(&gEncoderMux);

    encoderCountRemainder += (int)transitionDelta;
    while (encoderCountRemainder >= ENCODER_COUNTS_PER_DETENT) {
      volumeStep = min(volumeStep + 1, VOLUME_STEP_COUNT);
      encoderCountRemainder -= ENCODER_COUNTS_PER_DETENT;
    }
    while (encoderCountRemainder <= -ENCODER_COUNTS_PER_DETENT) {
      volumeStep = max(volumeStep - 1, 0);
      encoderCountRemainder += ENCODER_COUNTS_PER_DETENT;
    }

    const bool buttonReading = digitalRead(ENCODER_SW_PIN);
    if (buttonReading != lastButtonReading) {
      lastButtonChangeAt = startedAt;
      lastButtonReading = buttonReading;
    }
    if ((startedAt - lastButtonChangeAt) >= BUTTON_DEBOUNCE_MS && buttonReading != debouncedButtonState) {
      debouncedButtonState = buttonReading;
      if (!debouncedButtonState) {
        SharedState state = snapshotState();
        state.muted = !state.muted;
        publishAnalysisState(
          state.maskState,
          state.baselineP2P,
          state.targetGain,
          state.targetAdaptiveMix,
          volumeScaleFromStep(volumeStep),
          state.lastDominantFreqHz,
          state.bandTargets,
          state.lastPeakToPeak,
          state.lastRms,
          state.lastRefPeakToPeak,
          state.lastRefCorrelation,
          state.lastRefToMicRatio,
          state.muted
        );
      }
    }

    AnalysisFrame frame;
    captureAnalysisFrame(frame);

    const float adjustedPeakToPeak = (float)frame.peakToPeak * MIC_SENSITIVITY;
    if (!smoothingInitialized) {
      smoothedPeakToPeak = adjustedPeakToPeak;
      smoothedRms = frame.rms;
      smoothedDominantFreqHz = frame.dominantFreqHz;
      for (int band = 0; band < kBandCount; ++band) {
        smoothedBandPower[band] = frame.bandPowers[band];
      }
      smoothingInitialized = true;
    } else {
      smoothedPeakToPeak += (adjustedPeakToPeak - smoothedPeakToPeak) * 0.18f;
      smoothedRms += (frame.rms - smoothedRms) * 0.18f;
      smoothedDominantFreqHz += (frame.dominantFreqHz - smoothedDominantFreqHz) * 0.16f;
      for (int band = 0; band < kBandCount; ++band) {
        smoothedBandPower[band] += (frame.bandPowers[band] - smoothedBandPower[band]) * BAND_ANALYSIS_SMOOTH_ALPHA;
      }
    }

    frame.rms = smoothedRms;
    frame.dominantFreqHz = smoothedDominantFreqHz;

    SharedState state = snapshotState();
    state.volumeScale = volumeScaleFromStep(volumeStep);
    if (startupCalibrationStartedAt == 0) {
      startupCalibrationStartedAt = startedAt;
    }
    if (!startupCalibrated) {
      startupCalibrationSamples++;
      startupCalibrationSumP2P += smoothedPeakToPeak;
      startupCalibrationMaxP2P = fmaxf(startupCalibrationMaxP2P, smoothedPeakToPeak);

      const float calibrationAverage =
        startupCalibrationSumP2P / fmaxf(1.0f, (float)startupCalibrationSamples);
      const float calibrationBaseline =
        calibrationAverage * STARTUP_BASELINE_AVG_WEIGHT +
        startupCalibrationMaxP2P * STARTUP_BASELINE_MAX_WEIGHT;

      state.maskState = STATE_LISTENING;
      state.baselineP2P = fmaxf((float)MIN_TRIGGER_P2P, calibrationBaseline);
      state.targetGain = 0.0f;
      state.targetAdaptiveMix = 0.0f;
      for (int band = 0; band < kBandCount; ++band) {
        heldBandTargets[band] = 0.0f;
      }
      publishAnalysisState(
        state.maskState,
        state.baselineP2P,
        state.targetGain,
        state.targetAdaptiveMix,
        state.volumeScale,
        frame.dominantFreqHz,
        heldBandTargets,
        (int)smoothedPeakToPeak,
        frame.rms,
        frame.refPeakToPeak,
        frame.refCorrelation,
        frame.refToMicRatio,
        state.muted
      );

      if ((startedAt - startupCalibrationStartedAt) >= STARTUP_CALIBRATION_MS) {
        startupCalibrated = true;
        Serial.printf(
          "startup_calibration complete_ms=%lu avgP2P=%.1f maxP2P=%.1f baseline=%.1f\n",
          (unsigned long)(startedAt - startupCalibrationStartedAt),
          calibrationAverage,
          startupCalibrationMaxP2P,
          state.baselineP2P
        );
      }

      digitalWrite(STATUS_LED, (millis() / 250) % 2 ? HIGH : LOW);
      printStatus(snapshotState());

      const unsigned long elapsed = millis() - startedAt;
      if (elapsed < ANALYSIS_INTERVAL_MS) {
        vTaskDelay(pdMS_TO_TICKS(ANALYSIS_INTERVAL_MS - elapsed));
      } else {
        taskYIELD();
      }
      continue;
    }

    const float outputActivity = state.currentGain * state.currentAdaptiveMix * state.volumeScale;

    float feedbackMatch = 0.0f;
    float effectivePeakToPeak = smoothedPeakToPeak;
    if (SELF_FEEDBACK_GUARD_ENABLED && SELF_FEEDBACK_FREQ_MATCH_ENABLED && state.outputFocusHz > 0.0f && outputActivity > FEEDBACK_MIN_ACTIVITY) {
      const float freqDelta = fabsf(frame.dominantFreqHz - state.outputFocusHz);
      feedbackMatch = constrain(1.0f - (freqDelta / FEEDBACK_MATCH_HZ), 0.0f, 1.0f);
      const float feedbackSubtract = FEEDBACK_MAX_P2P_SUBTRACT * outputActivity * feedbackMatch;
      effectivePeakToPeak = fmaxf(0.0f, effectivePeakToPeak - feedbackSubtract);
    }

    const float adaptiveThreshold =
      fmaxf((float)MIN_TRIGGER_P2P, state.baselineP2P * TRIGGER_RATIO + TRIGGER_FLOOR);
    const bool likelySelfFeedback =
      SELF_FEEDBACK_GUARD_ENABLED &&
      (outputActivity > SELF_FEEDBACK_MIN_ACTIVITY) &&
      (frame.refCorrelation >= DUAL_MIC_FEEDBACK_CORRELATION) &&
      (frame.refToMicRatio >= DUAL_MIC_FEEDBACK_REF_RATIO) &&
      (smoothedPeakToPeak >= adaptiveThreshold * SELF_FEEDBACK_P2P_RATIO);
    if (likelySelfFeedback) {
      selfFeedbackFrames = min((uint8_t)(selfFeedbackFrames + 1), (uint8_t)SELF_FEEDBACK_RELEASE_FRAMES);
    } else if (selfFeedbackFrames > 0) {
      --selfFeedbackFrames;
    }
    const float dualMicFeedbackMatch =
      (SELF_FEEDBACK_GUARD_ENABLED &&
       (frame.refCorrelation >= DUAL_MIC_FEEDBACK_CORRELATION) &&
       (frame.refToMicRatio >= DUAL_MIC_FEEDBACK_REF_RATIO)) ? 1.0f : 0.0f;
    if (likelySelfFeedback) {
      effectivePeakToPeak = fminf(effectivePeakToPeak, adaptiveThreshold * SELF_FEEDBACK_P2P_CLAMP_RATIO);
    }

    frame.peakToPeak = (int)effectivePeakToPeak;
    const bool outputIsActive = outputActivity > SELF_FEEDBACK_MIN_ACTIVITY;
    const bool repeatedSelfFeedback = selfFeedbackFrames >= SELF_FEEDBACK_RELEASE_FRAMES;
    const bool cleanedMicQuiet =
      CLEAN_QUIET_RELEASE_ENABLED &&
      outputIsActive &&
      repeatedSelfFeedback;
    updateMaskingState(frame, state, startedAt, lastTriggerAt);

    if (cleanedMicQuiet) {
      if (cleanQuietSince == 0) {
        cleanQuietSince = startedAt;
      } else if ((startedAt - cleanQuietSince) >= CLEAN_QUIET_RELEASE_MS) {
        lastTriggerAt = 0;
        state.maskState = STATE_LISTENING;
        state.targetGain = 0.0f;
        state.targetAdaptiveMix = 0.0f;
      }
    } else {
      cleanQuietSince = 0;
    }

    float analysisBandPower[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float totalBandPower = 0.0f;
    float maxBandPower = 0.0f;
    for (int band = 0; band < kBandCount; ++band) {
      float adjustedPower = smoothedBandPower[band];
      if (SELF_FEEDBACK_GUARD_ENABLED && dualMicFeedbackMatch > 0.0f && state.outputFocusHz > 0.0f && outputActivity > FEEDBACK_MIN_ACTIVITY) {
        const float bandDelta = fabsf(kBands[band].centerHz - state.outputFocusHz);
        const float bandMatch = constrain(1.0f - (bandDelta / FEEDBACK_MATCH_HZ), 0.0f, 1.0f);
        const float selfMatch = fmaxf(feedbackMatch, dualMicFeedbackMatch);
        const float suppression = constrain(FEEDBACK_BAND_SUPPRESS * outputActivity * bandMatch * selfMatch, 0.0f, 0.9f);
        adjustedPower *= (1.0f - suppression);
      }
      analysisBandPower[band] = adjustedPower;
      totalBandPower += adjustedPower;
      maxBandPower = fmaxf(maxBandPower, adjustedPower);
    }

    float candidateTargets[kBandCount] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    if (totalBandPower > 0.0001f && maxBandPower > 0.0001f) {
      float maxWeight = 0.0f;
      for (int band = 0; band < kBandCount; ++band) {
        const float normalized = analysisBandPower[band] / totalBandPower;
        candidateTargets[band] = powf(normalized, 0.65f);
        maxWeight = fmaxf(maxWeight, candidateTargets[band]);
      }
      if (maxWeight > 0.0f) {
        for (int band = 0; band < kBandCount; ++band) {
          candidateTargets[band] = MIN_BAND_TARGET + (1.0f - MIN_BAND_TARGET) * (candidateTargets[band] / maxWeight);
        }
      }
    } else {
      for (int band = 0; band < kBandCount; ++band) {
        candidateTargets[band] = MIN_BAND_TARGET;
      }
    }

    const bool maskActive = (state.maskState == STATE_MASKING);

    for (int band = 0; band < kBandCount; ++band) {
      const float targetBand = (maskActive && !repeatedSelfFeedback) ? candidateTargets[band] : 0.0f;
      const float followAlpha = repeatedSelfFeedback ? 0.35f : BAND_TARGET_FOLLOW_ALPHA;
      heldBandTargets[band] += (targetBand - heldBandTargets[band]) * followAlpha;
      heldBandTargets[band] = constrain(heldBandTargets[band], 0.0f, 1.0f);
    }

    publishAnalysisState(
      state.maskState,
      state.baselineP2P,
      state.targetGain,
      state.targetAdaptiveMix,
      state.volumeScale,
      frame.dominantFreqHz,
      heldBandTargets,
      state.lastPeakToPeak,
      state.lastRms,
      frame.refPeakToPeak,
      frame.refCorrelation,
      frame.refToMicRatio,
      state.muted
    );

    if (maskActive) {
      ledOffAt = startedAt + 800;
    }
    digitalWrite(STATUS_LED, (millis() < ledOffAt) ? HIGH : LOW);

    printStatus(snapshotState());

    const unsigned long elapsed = millis() - startedAt;
    if (elapsed < ANALYSIS_INTERVAL_MS) {
      vTaskDelay(pdMS_TO_TICKS(ANALYSIS_INTERVAL_MS - elapsed));
    } else {
      taskYIELD();
    }
  }
}

void setup() {
  Serial.begin(115200);
  gBootId = esp_random();

#if OVERNIGHT_LOG_MODE
  Serial.printf(
    "overnight_boot,boot_id=%lu,reset_reason=%d,compile_date=%s,compile_time=%s\n",
    (unsigned long)gBootId,
    (int)esp_reset_reason(),
    __DATE__,
    __TIME__
  );
  Serial.println(
    "overnight_header,boot_id,uptime_ms,free_heap,state,p2p,baseline,threshold,inHz,outHz,gain,mix,volume,muted,maskActive,refP2P,refCorr,refRatio,rms,sampleRateHz,frameMs,detectToStateMs,stateToAudioMs,detectToAudioMs"
  );
#endif

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(REF_MIC_ADC_PIN, ADC_11db);

  setupEncoder();
  setupI2S();
  initializeAnalysisWindow();
  gToneFilter.setLowPass(OUTPUT_LOWPASS_HZ, 0.707f, SAMPLE_RATE, true);
  randomSeed(esp_random());

  xTaskCreatePinnedToCore(audioTask, "audioTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(analysisTask, "analysisTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
