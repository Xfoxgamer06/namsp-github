# NAM Amp for RG35XX SP (muOS)

Neural Amp Modeler player for the Anbernic RG35XX SP. Loads **NAM A2** captures (`.nam`), a cab impulse
response, a 3-band EQ and a noise gate, and **searches and downloads tones from TONE3000 directly on the
console** (sign-in by code from your phone, no browser needed). Full-duplex ALSA audio,
D-pad driven 640x480 UI. It is a lean re-implementation that uses the same NeuralAmpModelerCore as the
official TONE3000 plugin; it is not a build of the plugin itself (that one is JUCE-based, desktop-only).

**Status:** not yet run on a device. Unit-tested here: WAV loader, EQ, gate, cab convolution
(`tests/test_dsp.cpp`) and the whole TONE3000 client against a fake server: sign-in, token refresh, search
paths, model list, atomic downloads (`tests/test_t3k.cpp`). Only type-checked, never run: the SDL UI, the
ALSA engine, the NAM wrapper and the libcurl layer. Expect small fixes on the first real build.

## 0. First, measure (go / no-go)

Everything depends on whether the H700 (4x Cortex-A53) runs A2 in real time. The plugin repo ships a
benchmark for exactly that: copy `test/src/os_bench.cpp` from the plugin repo to `tests/os_bench.cpp`, build the
`a2_bench` target (step 3) and run it on the SP:

    ./a2_bench some_model.nam

It prints "% of one core (x realtime)". Aim for well under 50% of one core on a **lite** model.
If even lite is too heavy, no UI work will fix that.

## 1. Get the sources

NeuralAmpModelerCore must be the revision pinned by the plugin release (A2 support). Easiest is to clone
the real plugin repo with submodules and reuse its copy:

    git clone --recurse-submodules <tone3000-plugin repo> t3k
    cp -r t3k/plugin/NeuralAmpModelerCore  namsp/deps/NeuralAmpModelerCore

(The zip release has empty submodule folders, so use a git checkout.) Make sure
`deps/NeuralAmpModelerCore/Dependencies/eigen` and `.../nlohmann` are populated.

## 1b. TONE3000 key (for in-app download)

1. On tone3000.com: Settings > API Keys > create a **publishable** key (`t3k_pub_...`).
2. Put `t3k_key=t3k_pub_...` in `namsp.cfg`.
3. On the SP: Amp (NAM) > A > **[ Get from TONE3000 ]**. The first time it shows a code and an address:
   open the address on your phone, enter the code, approve. The session is stored in `t3k_auth.json`
   and renewed automatically.
4. Tabs (L/R): Trending / Favorites / Search. X opens the on-screen keyboard. A on a tone lists its A2
   models; A on a model downloads it to `models/<tone name>/` and loads it. "Download all" fetches the lot.
   The same works for cab IRs (Cab (IR) row), saved in `irs/`.

Needs Wi-Fi (the SP has it built in), HTTPS root certificates (put a `cacert.pem`, e.g. from curl.se/ca,
next to the executable if muOS has none) and a **correct console clock**: with a wrong date, TLS
certificate checks fail ("check the console date/time"). Use TONE3000 content according to its terms and
each creator's license.

## 2. Requirements on the device side

- USB-C **class-compliant audio interface**, used for BOTH capture and playback (two different cards
  have different clocks and drift into xruns). The SP has no audio input.
- A2 models only in practice (the official plugin rejects non-A2 files). A2 models are trained at 48 kHz,
  so keep `rate=48000`.

## 3. Build (aarch64, C++20)

Use an environment whose glibc matches your muOS (PortMaster / muOS aarch64 container), with
`libsdl2-dev libsdl2-ttf-dev libasound2-dev libcurl4-openssl-dev`:

    mkdir build && cd build
    SYSROOT=/path/to/sysroot cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64.cmake ..
    cmake --build . -j

Native build inside an aarch64 container needs no toolchain file. If `NAM_ENABLE_A2_FAST` fails on
aarch64, remove it from CMakeLists.txt (slower, but works).

## 4. Install

    NAMAmp/  NAMAmp.sh  namsp  font.ttf  namsp.cfg  models/*.nam  irs/*.wav  loops/*.wav

Edit `namsp.cfg` (see `namsp.cfg.example`): run `arecord -l` / `aplay -l` over SSH, card 1 -> `plughw:1,0`.

## Controls

| Button | Action |
|---|---|
| Up / Down | select row |
| Left / Right | adjust value (hold to repeat); on "Model size" toggles Lite/Full |
| L / R | adjust x5, page in file lists |
| A | open file list for Amp / Cab / Source |
| B | back |
| X | reset value |
| Y | quit (settings saved) |

Wrong buttons? `./namsp --debug-input` over SSH prints indices; set `btn_*` in `namsp.cfg`.

## Tuning

- Watch **DSP %**. Above about 85% you get crackles: use Lite, or raise `period` (256, 512).
- Changing Lite/Full reloads the model (it prewarms), which takes a moment.
- Latency is roughly `period / rate * 2` plus the interface.

## Not ported from the official plugin

Oversampling, stereo/dual-mono lanes, multi-core worker pool, effect chain, presets, tuner, MIDI mapping,
the JUCE UI, tone pictures/ratings, favoriting from the console.
You can still copy any `.nam` / `.wav` onto the SD card by hand.
