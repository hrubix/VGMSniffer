# VGM Sniffer — short manual

**VGM Sniffer** (Rubikon Lab) is a CLAP instrument that plays a VGM/VGZ file and **exposes one chip voice as audio + control signals** for your DAW (e.g. Bitwig Grid, samplers, synths).

It is not a full multi-channel mixer. You load a track, pick **one** channel (SCC 1–5 or PSG A/B/C), and route its outs into the rest of your patch.

---

## How it works

1. Add **VGM Sniffer** on a track.
2. Open the UI → **Load VGM** (`.vgm` / `.vgz`).
3. Select a channel: **PSG A/B/C** and/or **SCC 1–5** (only chips present in the file are shown).
4. Transport plays the VGM in sync with the host timeline. Looping VGMs wrap at the file’s loop point.
5. Use the stereo **Main** out to hear the soloed voice, and the mono outs / note out to drive other devices.

The UI tuner shows gate, note name, Hz, cents, volume, and CV preview for the selected channel.

---

## What you get in the DAW

### Audio outs

AY-3-8910 / YM2149 register map for PSG A / B / C: tone period **R0–R1 / R2–R3 / R4–R5**, shared noise period **R6**, mixer **R7**, volume **R8 / R9 / R10**, envelope period **R11–R12**, envelope shape **R13**.

| Port | Type | What it is | PSG registers (selected A/B/C) |
|------|------|------------|-------------------------------|
| **Main** | Stereo | Soloed voice mix (the channel you selected). | Tone+noise mix: period **R0–R5**, noise **R6**, mixer **R7**, vol **R8–R10** (+ env **R11–R13** if used). |
| **CV Hz** | Mono CV | Pitch as `Hz / 20000` (0…1). Recover Hz by ×20000. Gate off → 0. | Tone period **R0–R1** (A), **R2–R3** (B), or **R4–R5** (C); mute via **R7** tone bit. |
| **CV Vol** | Mono CV | Voice level 0…1 (SCC nibble/16, PSG linear gain). Gate off → 0. | Volume **R8** (A), **R9** (B), or **R10** (C); if bit 4 set, hardware envelope **R11–R13**. |
| **Wave** | Mono audio | Shape of the voice, level-normalized. **Wave × CV Vol** ≈ the real tick. | Same sources as **Main** for that voice (tone+noise after mixer/volume). |
| **PSG CV Noise** | Mono CV | Noise pitch as `Hz / 20000` (PSG only; else 0). | Shared noise period **R6**; noise enable in **R7**. |
| **PSG Tone** | Mono audio | Square-wave stem only (PSG). × **CV Vol** for real level. 0 on SCC. | Tone period **R0–R5** + **R7** tone bit; level from **R8–R10** / env. |
| **PSG Noise** | Mono audio | Noise stem only (PSG). × **CV Vol** for real level. 0 on SCC. | Noise **R6** + **R7** noise bit; level from **R8–R10** / env. |
| **PSG Mixer** | Mono CV | Song mixer `(R7 & 0x3F) / 63`. Bits 6 and 7 (I/O direction) are not included. Live for the whole PSG, including while an SCC channel is selected. 0 when the file has no PSG. | **R7** bits 0–5 (tone A/B/C, noise A/B/C). 0 = that source on. |
| **PSG Env Shape** | Mono CV | Envelope shape `R13 / 15`. 0 is shape 0, not “no envelope”. | **R13** bits 0–3. |
| **PSG Env Period** | Mono CV | Envelope period `(R11 + R12<<8) / 65535`. | **R11** fine, **R12** coarse. |

### Note out

| Port | What it is |
|------|------------|
| **SCC Pitch** | MIDI / CLAP notes from the selected voice’s pitch (note on/off + optional microtuning). Optional MIDI pitch bend (±2 st) for fine cents. |

Typical Bitwig uses: **CV Hz** → Freq→Pitch / oscillators; **CV Vol** → amp or gates; **Wave / Tone / Noise** → audio processing; **Note out** → Note Receiver → another synth.

---

## Tips

- Pick the channel that carries the part you want (kick noise, bass tone, lead SCC, etc.).
- For “pitch CV into a sampler,” use **CV Hz** (not audio Tone) and convert with ×20000 then Freq→Pitch.
- **Tone** is the square audio; **CV Hz** is only the pitch number — they are not interchangeable.
- Avoid feedback loops (routing Sniffer through something that feeds back into the same track).

Vendor: [rubikonlab.nl](https://rubikonlab.nl)
