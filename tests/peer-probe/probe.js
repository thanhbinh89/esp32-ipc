/*
 * Headless replacement for tests/sepfy.github.io/libpeer/index.html.
 *
 * Speaks the same JSON-RPC-over-MQTT signaling the page does (offer/answer on
 * /public/<id>/invoke and /result), then reports what actually arrives on the
 * video track. The page can only show whether the picture looks right; the two
 * numbers that matter here are invisible in a <video> element:
 *
 *   fps      - frames per second measured from RTP marker bits, i.e. the rate
 *              the device really sends.
 *   ts_fps   - 90000 / (mean RTP timestamp step), i.e. the rate the device
 *              *claims* to send. libpeer derives the step from
 *              CONFIG_CODEC_H264_FPS, so it is a constant, not a measurement.
 *
 * When those two disagree the receiver's timeline is wrong by that ratio and it
 * asks for a keyframe forever, which is the failure this probe exists to catch.
 *
 * Usage: node probe.js [deviceId] [seconds]
 */

const mqtt = require('mqtt');
const { RTCPeerConnection, RTCRtpCodecParameters } = require('werift');

const deviceId = process.argv[2] || 'striped-lazy-panda';
const runSeconds = Number(process.argv[3] || 60);

const invokeTopic = `/public/${deviceId}/invoke`;
const resultTopic = `/public/${deviceId}/result`;
const offerId = Math.floor(Math.random() * 1000) + 1;
const answerId = Math.floor(Math.random() * 1000) + 1;

const stats = {
  packets: 0,
  bytes: 0,
  frames: 0,
  keyframes: 0,
  tsSteps: [],
  lastTs: null,
  firstFrameAt: null,
};

function log(...args) {
  const t = ((Date.now() - startedAt) / 1000).toFixed(1).padStart(6);
  console.log(`[${t}s]`, ...args);
}

const startedAt = Date.now();

/* werift starts with no codecs at all and answers "negotiate codecs failed" to
 * anything it is offered, so these have to mirror the device's SDP exactly --
 * H.264 on 102 with packetization-mode=1, PCMA on 8. */
const pc = new RTCPeerConnection({
  /* The same pair the device uses (see CONFIG_STUN_URL / CONFIG_TURN_* in
   * sdkconfig). This host is not on the device's LAN, so without a relay both
   * sides only ever offer candidates the other cannot reach. */
  iceServers: [
    { urls: 'stun:stun-connect.fcam.vn:3478' },
    {
      urls: 'turn:turn-connect.fcam.vn:3478',
      username: 'turnuser',
      credential: 'camfptvnturn133099',
    },
  ],
  codecs: {
    video: [
      new RTCRtpCodecParameters({
        mimeType: 'video/H264',
        clockRate: 90000,
        payloadType: 102,
        rtcpFeedback: [{ type: 'nack' }, { type: 'nack', parameter: 'pli' }],
        parameters: 'profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1',
      }),
    ],
    audio: [
      new RTCRtpCodecParameters({ mimeType: 'audio/PCMA', clockRate: 8000, payloadType: 8 }),
    ],
  },
});

pc.onTrack.subscribe((track) => {
  log(`track: ${track.kind} ${track.codec ? track.codec.mimeType : ''}`);
  if (track.kind !== 'video') return;

  track.onReceiveRtp.subscribe((rtp) => {
    stats.packets++;
    stats.bytes += rtp.payload.length;

    /* An H.264 IDR reaches us either as a bare NAL type 5 or as the first
     * fragment of a FU-A carrying one; both matter for "did a keyframe land". */
    const b = rtp.payload;
    if (b.length > 1) {
      const nal = b[0] & 0x1f;
      if (nal === 5) stats.keyframes++;
      else if (nal === 28 && (b[1] & 0x80) && (b[1] & 0x1f) === 5) stats.keyframes++;
    }

    if (rtp.header.marker) {
      stats.frames++;
      if (stats.firstFrameAt === null) stats.firstFrameAt = Date.now();
      if (stats.lastTs !== null) {
        const step = (rtp.header.timestamp - stats.lastTs) >>> 0;
        if (step > 0 && step < 90000) stats.tsSteps.push(step);
      }
      stats.lastTs = rtp.header.timestamp;
    }
  });
});

pc.iceConnectionStateChange.subscribe((s) => log(`ice: ${s}`));
pc.connectionStateChange.subscribe((s) => log(`pc: ${s}`));

const client = mqtt.connect('mqtt://broker.emqx.io:1883');

client.on('connect', () => {
  log('mqtt connected, subscribing', resultTopic);
  client.subscribe(resultTopic, () => {
    log('sending offer request id=' + offerId);
    client.publish(invokeTopic, JSON.stringify({ jsonrpc: '2.0', method: 'offer', id: offerId }));
  });
});

client.on('message', async (topic, message) => {
  let msg;
  try {
    msg = JSON.parse(message.toString());
  } catch {
    return;
  }
  if (msg.id !== offerId || !msg.result) return;

  log(`offer received, ${msg.result.length} bytes of SDP`);
  await pc.setRemoteDescription({ type: 'offer', sdp: msg.result });
  const answer = await pc.createAnswer();
  await pc.setLocalDescription(answer);

  /* Publish only once gathering is done, exactly as the reference page does in
   * onicegatheringstatechange. libpeer does not trickle: it reads the candidate
   * lines out of the answer it is handed and never looks again, so an answer
   * sent before gathering completes leaves it with nothing to connect to. */
  if (pc.iceGatheringState !== 'complete') {
    log('waiting for ICE gathering...');
    await new Promise((resolve) => {
      const done = pc.iceGatheringStateChange.subscribe((s) => {
        if (s === 'complete') {
          done.unsubscribe();
          resolve();
        }
      });
      setTimeout(resolve, 10000);
    });
  }

  /* libpeer stores remote candidates in a fixed array of AGENT_MAX_CANDIDATES
   * (10, see agent.h) and silently keeps only the first that fit. werift offers
   * one host candidate per local interface -- loopback, docker0, the VPN -- and
   * on this host those alone overflow the array, so the srflx and relay entries
   * that could actually reach the device never get stored. Send only the ones
   * that can work. */
  const sdp = pc.localDescription.sdp
    .split('\r\n')
    .filter((l) => !(l.startsWith('a=candidate:') && l.includes('typ host')))
    .join('\r\n');

  const kept = (sdp.match(/^a=candidate:.*$/gm) || []);
  log(`publishing answer id=${answerId} with ${kept.length} candidates (host filtered out)`);
  kept.slice(0, 4).forEach((c) => log('  ' + c.slice(0, 80)));
  client.publish(
    invokeTopic,
    JSON.stringify({ jsonrpc: '2.0', method: 'answer', params: sdp, id: answerId })
  );
});

let prev = { packets: 0, bytes: 0, frames: 0, keyframes: 0 };
const ticker = setInterval(() => {
  const d = {
    packets: stats.packets - prev.packets,
    bytes: stats.bytes - prev.bytes,
    frames: stats.frames - prev.frames,
    keyframes: stats.keyframes - prev.keyframes,
  };
  prev = { packets: stats.packets, bytes: stats.bytes, frames: stats.frames, keyframes: stats.keyframes };
  if (d.packets === 0) return;
  log(
    `fps=${d.frames} pkts=${d.packets} kbps=${Math.round((d.bytes * 8) / 1000)} idr=${d.keyframes}`
  );
}, 1000);

setTimeout(() => {
  clearInterval(ticker);
  const span = stats.firstFrameAt ? (Date.now() - stats.firstFrameAt) / 1000 : 0;
  const meanStep = stats.tsSteps.length
    ? stats.tsSteps.reduce((a, b) => a + b, 0) / stats.tsSteps.length
    : 0;

  console.log('\n================ summary ================');
  console.log(`frames            : ${stats.frames} over ${span.toFixed(1)}s`);
  console.log(`measured fps      : ${span ? (stats.frames / span).toFixed(2) : 'n/a'}`);
  console.log(`RTP timestamp step: ${meanStep.toFixed(0)} ticks`);
  console.log(`declared fps      : ${meanStep ? (90000 / meanStep).toFixed(2) : 'n/a'}`);
  console.log(`bitrate           : ${span ? Math.round((stats.bytes * 8) / span / 1000) : 0} kbps`);
  console.log(`keyframes         : ${stats.keyframes}`);
  if (span && meanStep) {
    const ratio = stats.frames / span / (90000 / meanStep);
    console.log(`measured/declared : ${ratio.toFixed(2)}x  ${Math.abs(ratio - 1) < 0.15 ? 'OK' : 'MISMATCH -- receiver timeline will drift'}`);
  }
  process.exit(0);
}, runSeconds * 1000);
