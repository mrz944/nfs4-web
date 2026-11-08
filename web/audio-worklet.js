// Drains the runtime's audio ring (interleaved s16 stereo in shared wasm memory).
// Ring layout (dsound.c WebAudio): u32 rd, u32 wr, u32 cap, u32 rate, s16 data[2*cap]
// Each call also bumps the runtime's heartbeat word and wakes its waiter: this thread runs
// every 128 samples on time, which the game's 1 ms timers need (a browser on Windows can't
// sleep shorter than a 15.6 ms tick, see user32.c).
class RingPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.calls = 0; this.under = 0; this.t = currentTime;
    this.port.onmessage = (e) => {
      const { buffer, ptr, beat } = e.data;
      if (beat) this.beat = new Int32Array(buffer, beat, 1);
      this.u32 = new Uint32Array(buffer, ptr, 4);
      this.cap = this.u32[2];
      this.pcm = new Int16Array(buffer, ptr + 16, 2 * this.cap);
      this.ready = true;
    };
  }
  process(_inputs, outputs) {
    if (this.beat) { Atomics.add(this.beat, 0, 1); Atomics.notify(this.beat, 0); }
    const out = outputs[0], L = out[0], R = out[1] || out[0];
    if (!this.ready) { L.fill(0); if (R !== L) R.fill(0); return true; }
    const rd0 = Atomics.load(this.u32, 0);
    let rd = rd0;
    const wr = Atomics.load(this.u32, 1), mask = this.cap - 1;
    for (let i = 0; i < L.length; i++) {
      if (rd === wr) { L[i] = 0; R[i] = 0; this.under++; continue; }   // underrun: silence
      const j = (rd & mask) * 2;
      L[i] = this.pcm[j] / 32768; R[i] = this.pcm[j + 1] / 32768;
      rd = (rd + 1) >>> 0;
    }
    Atomics.compareExchange(this.u32, 0, rd0, rd);   // the game may have dropped old samples meanwhile: keep its value
    this.calls++;
    if (currentTime - this.t >= 10) {        // every 10 s of audio time: how it went (main.js logs it if bad)
      this.port.postMessage({ calls: this.calls, under: this.under, secs: currentTime - this.t });
      this.calls = 0; this.under = 0; this.t = currentTime;
    }
    return true;
  }
}
registerProcessor('ring-player', RingPlayer);
