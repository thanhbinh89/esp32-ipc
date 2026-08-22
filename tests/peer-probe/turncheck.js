/* Does turn-connect.fcam.vn still hand out a relay allocation? */
const { RTCPeerConnection, RTCRtpCodecParameters } = require('werift');
const pc = new RTCPeerConnection({
  iceServers: [
    { urls: 'stun:stun-connect.fcam.vn:3478' },
    { urls: 'turn:turn-connect.fcam.vn:3478', username: 'turnuser', credential: 'camfptvnturn133099' },
  ],
  codecs: { video: [new RTCRtpCodecParameters({ mimeType: 'video/H264', clockRate: 90000, payloadType: 102 })] },
});
(async () => {
  pc.addTransceiver('video', { direction: 'recvonly' });
  await pc.setLocalDescription(await pc.createOffer());
  const c = (pc.localDescription.sdp.match(/^a=candidate:.*$/gm) || []);
  const types = {};
  c.forEach((l) => { const t = (l.match(/typ (\w+)/) || [])[1]; types[t] = (types[t] || 0) + 1; });
  console.log('candidates by type:', JSON.stringify(types));
  console.log(types.relay ? 'TURN OK - relay allocation granted' : 'TURN FAILED - no relay candidate');
  process.exit(0);
})();
