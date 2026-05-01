#!/usr/bin/env python3
"""Simulation study for the adaptive sleep masking algorithm.

This is not a hardware emulator. It is a signal-processing demonstration of the
core idea used in sleep_masking_multiband.ino:
  1. analyze microphone audio with short-time FFT frames,
  2. estimate dominant disturbance bands,
  3. generate colored noise shaped toward those bands,
  4. show that the disturbance becomes less spectrally prominent.
"""

from __future__ import annotations

import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(__file__).with_name(".mplconfig")))

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np


FS = 16_000
FRAME = 256
HOP = 128
FFT_MIN_HZ = 80.0
FFT_MAX_HZ = 5_000.0

BANDS = np.array(
    [
        (80.0, 180.0, 130.0),
        (180.0, 360.0, 260.0),
        (360.0, 720.0, 520.0),
        (720.0, 1400.0, 1020.0),
        (1400.0, 2800.0, 2050.0),
        (2800.0, 5000.0, 3800.0),
    ]
)


def db(x: np.ndarray, floor: float = 1e-12) -> np.ndarray:
    return 10.0 * np.log10(np.maximum(x, floor))


def make_disturbance(duration_s: float = 6.0, seed: int = 4) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    t = np.arange(int(FS * duration_s)) / FS

    room = 0.015 * rng.standard_normal(t.size)

    # Three example events: a low thump, a tonal whine, and a higher broadband burst.
    low_env = np.exp(-((t - 1.3) / 0.18) ** 2)
    low_event = 0.24 * low_env * np.sin(2 * np.pi * 145 * t)

    tone_env = ((t > 2.35) & (t < 3.8)).astype(float)
    tone_env = np.convolve(tone_env, np.hanning(801) / np.sum(np.hanning(801)), mode="same")
    tone_event = 0.10 * tone_env * np.sin(2 * np.pi * 820 * t)

    burst_env = np.exp(-((t - 4.65) / 0.26) ** 2)
    burst_noise = rng.standard_normal(t.size)
    burst_fft = np.fft.rfft(burst_noise)
    freqs = np.fft.rfftfreq(t.size, 1 / FS)
    band_shape = np.exp(-0.5 * ((freqs - 2500.0) / 650.0) ** 2)
    high_event = 0.10 * burst_env * np.fft.irfft(burst_fft * band_shape, n=t.size)
    high_event /= np.max(np.abs(high_event)) + 1e-9
    high_event *= 0.14

    disturbance = room + low_event + tone_event + high_event
    return t, disturbance


def source_color_mix(focus_hz: float) -> tuple[float, float, float]:
    low_bias = np.clip((700.0 - focus_hz) / 600.0, 0.0, 1.0)
    high_bias = np.clip((focus_hz - 1300.0) / 1800.0, 0.0, 1.0)
    brown = 0.10 + 0.70 * low_bias
    white = 0.10 + 0.70 * high_bias
    pink = 1.00
    total = white + pink + brown
    return white / total, pink / total, brown / total


def colored_noise_spectrum(freqs: np.ndarray, focus_hz: float, rng: np.random.Generator) -> np.ndarray:
    white_mix, pink_mix, brown_mix = source_color_mix(focus_hz)
    random_phase = rng.uniform(0.0, 2.0 * np.pi, freqs.size)
    random_complex = np.exp(1j * random_phase)
    safe_freqs = np.maximum(freqs, 20.0)
    color = white_mix + pink_mix / np.sqrt(safe_freqs / 200.0) + brown_mix / (safe_freqs / 200.0)
    return random_complex * color


def analyze_and_generate_mask(disturbance: np.ndarray, seed: int = 9) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    window = np.hanning(FRAME)
    freqs = np.fft.rfftfreq(FRAME, 1 / FS)
    valid = (freqs >= FFT_MIN_HZ) & (freqs <= FFT_MAX_HZ)

    output = np.zeros_like(disturbance)
    ola_weight = np.zeros_like(disturbance)
    times = []
    p2p = []
    baseline = 0.08
    thresholds = []
    dominant_hz = []
    output_focus_hz = []
    band_targets = []
    active = []
    smoothed_targets = np.zeros(len(BANDS))

    for start in range(0, len(disturbance) - FRAME, HOP):
        frame = disturbance[start : start + FRAME]
        frame_p2p = float(np.max(frame) - np.min(frame))
        threshold = max(0.09, baseline * 1.6 + 0.025)
        triggered = frame_p2p > threshold

        if not triggered:
            baseline = 0.985 * baseline + 0.015 * frame_p2p

        spec = np.fft.rfft(frame * window)
        power = np.abs(spec) ** 2
        power[~valid] = 0.0

        band_power = []
        for lo, hi, _center in BANDS:
            bins = (freqs >= lo) & (freqs < hi)
            band_power.append(float(np.sum(power[bins])))
        band_power = np.array(band_power)

        if np.max(band_power) > 1e-12:
            targets = (band_power / np.sum(band_power)) ** 0.65
            targets /= np.max(targets)
        else:
            targets = np.zeros(len(BANDS))

        smoothed_targets += 0.20 * ((targets if triggered else 0.0) - smoothed_targets)
        focus = float(np.sum(smoothed_targets * BANDS[:, 2]) / (np.sum(smoothed_targets) + 1e-9))

        noise_spec = colored_noise_spectrum(freqs, focus if focus > 0 else 600.0, rng)
        band_shape = np.zeros_like(freqs)
        for band_idx, (lo, hi, _center) in enumerate(BANDS):
            bins = (freqs >= lo) & (freqs < hi)
            band_shape[bins] = smoothed_targets[band_idx]
        noise_frame = np.fft.irfft(noise_spec * band_shape, n=FRAME)
        noise_frame *= window
        noise_frame /= np.max(np.abs(noise_frame)) + 1e-9
        noise_frame *= 0.20 if triggered else 0.0

        output[start : start + FRAME] += noise_frame
        ola_weight[start : start + FRAME] += window

        dom_bin = int(np.argmax(power))
        times.append((start + FRAME / 2) / FS)
        p2p.append(frame_p2p)
        thresholds.append(threshold)
        dominant_hz.append(freqs[dom_bin])
        output_focus_hz.append(focus)
        band_targets.append(smoothed_targets.copy())
        active.append(1.0 if triggered else 0.0)

    output /= np.maximum(ola_weight, 1e-6)
    return {
        "mask": output,
        "times": np.array(times),
        "p2p": np.array(p2p),
        "baseline": np.full(len(times), baseline),
        "threshold": np.array(thresholds),
        "dominant_hz": np.array(dominant_hz),
        "output_focus_hz": np.array(output_focus_hz),
        "band_targets": np.array(band_targets),
        "active": np.array(active),
    }


def spectrogram(signal: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    window = np.hanning(FRAME)
    freqs = np.fft.rfftfreq(FRAME, 1 / FS)
    columns = []
    times = []
    for start in range(0, len(signal) - FRAME, HOP):
        spec = np.fft.rfft(signal[start : start + FRAME] * window)
        columns.append(db(np.abs(spec) ** 2))
        times.append((start + FRAME / 2) / FS)
    return np.array(times), freqs, np.array(columns).T


def band_prominence(signal: np.ndarray, focus_hz: float, width_hz: float = 40.0) -> float:
    freqs = np.fft.rfftfreq(signal.size, 1 / FS)
    power = np.abs(np.fft.rfft(signal * np.hanning(signal.size))) ** 2
    focus_bins = np.abs(freqs - focus_hz) <= width_hz
    nearby_bins = (freqs > focus_hz - 450.0) & (freqs < focus_hz + 450.0) & ~focus_bins
    peak_db = db(np.array([np.mean(power[focus_bins])]))[0]
    floor_db = db(np.array([np.median(power[nearby_bins])]))[0]
    return peak_db - floor_db


def save_plots(out_dir: Path) -> None:
    out_dir.mkdir(exist_ok=True)
    t, disturbance = make_disturbance()
    result = analyze_and_generate_mask(disturbance)
    mask = result["mask"]
    combined = disturbance + mask

    spec_t, freqs, spec_before = spectrogram(disturbance)
    _spec_t, _freqs, spec_after = spectrogram(combined)

    fig, axes = plt.subplots(3, 1, figsize=(11, 9), constrained_layout=True)
    axes[0].plot(t, disturbance, label="disturbance", linewidth=1.0)
    axes[0].plot(t, mask, label="adaptive masking output", linewidth=0.9, alpha=0.85)
    axes[0].set_title("Synthetic disturbance and adaptive masking output")
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Amplitude")
    axes[0].legend(loc="upper right")

    axes[1].plot(result["times"], result["p2p"], label="mic p2p")
    axes[1].plot(result["times"], result["threshold"], label="adaptive threshold")
    axes[1].fill_between(result["times"], 0, result["active"] * np.max(result["p2p"]), alpha=0.16, label="mask active")
    axes[1].set_title("Adaptive baseline/threshold trigger behavior")
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Frame level")
    axes[1].legend(loc="upper right")

    axes[2].plot(result["times"], result["dominant_hz"], label="input dominant Hz")
    axes[2].plot(result["times"], result["output_focus_hz"], label="output focus Hz")
    axes[2].set_title("FFT-estimated disturbance frequency vs. masking target")
    axes[2].set_xlabel("Time (s)")
    axes[2].set_ylabel("Frequency (Hz)")
    axes[2].set_ylim(0, 4200)
    axes[2].legend(loc="upper right")
    fig.savefig(out_dir / "adaptive_masking_timeseries.png", dpi=180)
    plt.close(fig)

    fig, axes = plt.subplots(2, 1, figsize=(11, 8), constrained_layout=True)
    im0 = axes[0].pcolormesh(spec_t, freqs, spec_before, shading="auto", cmap="magma")
    axes[0].set_title("Before masking: disturbance spectrogram")
    axes[0].set_ylabel("Frequency (Hz)")
    axes[0].set_ylim(0, 5000)
    fig.colorbar(im0, ax=axes[0], label="Power (dB)")

    im1 = axes[1].pcolormesh(spec_t, freqs, spec_after, shading="auto", cmap="magma")
    axes[1].set_title("After masking: disturbance blended into shaped noise")
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Frequency (Hz)")
    axes[1].set_ylim(0, 5000)
    fig.colorbar(im1, ax=axes[1], label="Power (dB)")
    fig.savefig(out_dir / "adaptive_masking_spectrograms.png", dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(11, 4.8), constrained_layout=True)
    for idx in range(len(BANDS)):
        lo, hi, center = BANDS[idx]
        ax.plot(result["times"], result["band_targets"][:, idx], label=f"{int(lo)}-{int(hi)} Hz")
    ax.set_title("FFT band targets used to shape the masking noise")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Normalized target")
    ax.legend(ncol=3, loc="upper right")
    fig.savefig(out_dir / "adaptive_masking_band_targets.png", dpi=180)
    plt.close(fig)

    tone_slice = (t > 2.35) & (t < 3.8)
    before_prom = band_prominence(disturbance[tone_slice], 820.0)
    after_prom = band_prominence(combined[tone_slice], 820.0)
    reduction = before_prom - after_prom

    print("Simulation complete.")
    print(f"Saved plots to: {out_dir}")
    print(f"820 Hz tone prominence before masking: {before_prom:.1f} dB")
    print(f"820 Hz tone prominence after masking:  {after_prom:.1f} dB")
    print(f"Prominence reduction:                 {reduction:.1f} dB")


def main() -> None:
    save_plots(Path(__file__).with_name("simulation_outputs"))


if __name__ == "__main__":
    main()
