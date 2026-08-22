const mqtt = require('mqtt');
const id = 'striped-lazy-panda';
const offerId = Math.floor(Math.random()*1000)+1;
const c = mqtt.connect('mqtt://broker.emqx.io:1883');
c.on('connect', () => c.subscribe(`/public/${id}/result`, () => {
  c.publish(`/public/${id}/invoke`, JSON.stringify({jsonrpc:'2.0',method:'offer',id:offerId}));
}));
c.on('message', (t,m) => {
  const msg = JSON.parse(m.toString());
  if (msg.id === offerId && msg.result) { console.log(msg.result); process.exit(0); }
});
setTimeout(()=>{console.log('timeout');process.exit(1);}, 20000);
