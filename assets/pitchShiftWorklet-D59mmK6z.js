// Delay-line pitch shifter for the music chain (stands in for AVAudioUnitTimePitch's pitch control; tempo comes
// from the media element's pitch-preserving playbackRate).
//
// Two read taps sweep across a short delay window half a cycle apart; each fades in and out with a sin² window, so
// the pair always sums to unity and a tap only jumps back when its weight is zero. At a ratio of 1 the processor
// crossfades to the dry input so neutral playback has no comb filtering or added latency.

class PitchShiftProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [{ name: 'ratio', defaultValue: 1, minValue: 0.25, maxValue: 4, automationRate: 'k-rate' }];
  }

  constructor() {
    super();
    this.window = Math.round(sampleRate * 0.055);
    this.size = this.window * 2 + 8;
    this.buffers = [];
    this.write = 0;
    this.phase = 0;
    this.wet = 0;
    this.wetStep = 1 / (sampleRate * 0.06);
  }

  read(buffer, position) {
    const size = this.size;
    let index = position % size;
    if (index < 0) index += size;
    const base = Math.floor(index);
    const fraction = index - base;
    const next = base + 1 >= size ? 0 : base + 1;
    return buffer[base] + (buffer[next] - buffer[base]) * fraction;
  }

  process(inputs, outputs, parameters) {
    const input = inputs[0];
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const channels = output.length;
    while (this.buffers.length < channels) this.buffers.push(new Float32Array(this.size));
    const frames = output[0].length;
    const ratio = parameters.ratio[0];
    const shifting = Math.abs(ratio - 1) > 0.0005;
    const step = (1 - ratio) / this.window;
    const window = this.window;

    for (let i = 0; i < frames; i++) {
      for (let c = 0; c < channels; c++) {
        const source = input && (input[c] || input[0]);
        this.buffers[c][this.write] = source ? source[i] : 0;
      }
      const target = shifting ? 1 : 0;
      if (this.wet < target) this.wet = Math.min(target, this.wet + this.wetStep);
      else if (this.wet > target) this.wet = Math.max(target, this.wet - this.wetStep);

      const p1 = this.phase;
      const p2 = p1 + 0.5 >= 1 ? p1 - 0.5 : p1 + 0.5;
      const s1 = Math.sin(Math.PI * p1);
      const g1 = s1 * s1;
      const g2 = 1 - g1;
      const d1 = p1 * window + 1;
      const d2 = p2 * window + 1;
      for (let c = 0; c < channels; c++) {
        const buffer = this.buffers[c];
        const dry = buffer[this.write];
        if (this.wet <= 0) {
          output[c][i] = dry;
          continue;
        }
        const shifted = this.read(buffer, this.write - d1) * g1 + this.read(buffer, this.write - d2) * g2;
        output[c][i] = dry * (1 - this.wet) + shifted * this.wet;
      }
      if (shifting) {
        this.phase += step;
        if (this.phase >= 1) this.phase -= 1;
        else if (this.phase < 0) this.phase += 1;
      }
      this.write = this.write + 1 >= this.size ? 0 : this.write + 1;
    }
    return true;
  }
}

registerProcessor('stak-pitch-shift', PitchShiftProcessor);
