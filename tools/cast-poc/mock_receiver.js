// R2 offline self-check: a minimal CastV2 receiver that speaks just enough of
// the protocol for cast_sender.js to run end-to-end without a TV.
//
// This does NOT validate Sony CAF behaviour (that still needs the real set).
// It validates OUR side of the wire: envelope fields (source/destination/
// namespace/requestId), LAUNCH -> transportId handoff, heartbeat, LOAD with
// streamType=LIVE, and MEDIA_STATUS -> JSONL -> judge_r1.py verdict.
//
// The mock advances a synthetic playhead 1 s per wall-clock second, so the
// judge must genuinely PASS on a healthy stream. A deliberately frozen mode
// (--freeze-after N) is provided to prove the same chain can also FAIL.
//
// Usage:
//   node mock_receiver.js --port 18009 --seconds 100
//   node mock_receiver.js --port 18009 --freeze-after 10   # negative control
//
// Exit: 0 after the observation window; connection errors go to stderr.

'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const CastServer = require('castv2').Server;

const NS_CONNECTION = 'urn:x-cast:com.google.cast.tp.connection';
const NS_RECEIVER = 'urn:x-cast:com.google.cast.receiver';
const NS_HEARTBEAT = 'urn:x-cast:com.google.cast.tp.heartbeat';
const NS_MEDIA = 'urn:x-cast:com.google.cast.media';
const APP_ID = 'CC1AD845';
const TRANSPORT_ID = 'mock-transport-0';
const SENDER_ID = 'sender-0';
const RECEIVER_ID = 'receiver-0';

function parseArgs(argv) {
  const args = {
    port: 18009,
    seconds: 100,
    certDir: path.join(__dirname, '.mock-cert'),
    freezeAfter: null,
  };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    const value = argv[++i];
    if (key === '--port') args.port = Number(value);
    else if (key === '--seconds') args.seconds = Number(value);
    else if (key === '--cert-dir') args.certDir = path.resolve(value);
    else if (key === '--freeze-after') args.freezeAfter = Number(value);
    else if (key === '--help') {
      console.log('Usage: node mock_receiver.js [--port 18009] [--seconds 100] [--freeze-after N] [--cert-dir DIR]');
      process.exit(0);
    }
  }
  return args;
}

function ensureCert(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const key = path.join(dir, 'key.pem');
  const cert = path.join(dir, 'cert.pem');
  if (!fs.existsSync(key) || !fs.existsSync(cert)) {
    const openssl = process.env.OPENSSL_BIN || 'openssl';
    const result = spawnSync(openssl, [
      'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
      '-subj', '/CN=localhost', '-keyout', key, '-out', cert,
    ], { encoding: 'utf8' });
    if (result.status !== 0) {
      throw new Error('openssl failed to create self-signed cert: ' +
        String(result.stderr || (result.error && result.error.message) || result.status));
    }
  }
  return { key: fs.readFileSync(key), cert: fs.readFileSync(cert) };
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const tlsOptions = ensureCert(args.certDir);
  const server = new CastServer(tlsOptions);

  let statusTimer = null;
  let pongs = 0;
  let launches = 0;
  let loads = 0;

  server.listen(args.port, '127.0.0.1', () => {
    console.log(`MOCK RECEIVER READY tls://127.0.0.1:${args.port} (cert=${args.certDir})`);
  });

  server.on('error', (err) => {
    console.error('MOCK SERVER ERROR', err);
    process.exitCode = 1;
  });

  server.on('message', (clientId, sourceId, destinationId, namespace, text) => {
    let msg;
    try {
      msg = JSON.parse(text);
    } catch (_) {
      return;
    }

    // Platform heartbeat. A real device answers PONG on the same channel with
    // source/destination swapped; the sender treats silence as a dead session.
    if (namespace === NS_HEARTBEAT && msg.type === 'PING') {
      pongs++;
      server.send(clientId, RECEIVER_ID, SENDER_ID, NS_HEARTBEAT,
        JSON.stringify({ type: 'PONG' }));
      return;
    }

    // LAUNCH -> RECEIVER_STATUS carrying the app transportId. The sender only
    // opens the media channel after it sees this field, so a missing id is a
    // realistic failure mode we want the offline check to expose.
    if (namespace === NS_RECEIVER && msg.type === 'LAUNCH') {
      launches++;
      console.log(`MOCK LAUNCH appId=${msg.appId}`);
      server.send(clientId, RECEIVER_ID, SENDER_ID, NS_RECEIVER, JSON.stringify({
        type: 'RECEIVER_STATUS',
        requestId: msg.requestId,
        status: {
          applications: [{
            appId: APP_ID,
            transportId: TRANSPORT_ID,
            statusText: 'POC Mock Receiver',
          }],
        },
      }));
      return;
    }

    // LOAD -> BUFFERING then one PLAYING status per second. Mirrors the shape
    // judge_r1.py consumes; --freeze-after N is the negative control.
    if (namespace === NS_MEDIA && msg.type === 'LOAD') {
      loads++;
      console.log(`MOCK LOAD contentId=${msg.media && msg.media.contentId} ` +
        `streamType=${msg.media && msg.media.streamType}`);
      let playhead = 0;
      server.send(clientId, TRANSPORT_ID, SENDER_ID, NS_MEDIA, JSON.stringify({
        type: 'MEDIA_STATUS',
        requestId: msg.requestId,
        status: [{ mediaSessionId: 1, playerState: 'BUFFERING', currentTime: 0 }],
      }));
      if (statusTimer) clearInterval(statusTimer);
      statusTimer = setInterval(() => {
        playhead++;
        const frozen = args.freezeAfter !== null && playhead > args.freezeAfter;
        const currentTime = frozen ? args.freezeAfter : playhead;
        server.send(clientId, TRANSPORT_ID, SENDER_ID, NS_MEDIA, JSON.stringify({
          type: 'MEDIA_STATUS',
          status: [{
            mediaSessionId: 1,
            playerState: 'PLAYING',
            currentTime,
            playbackRate: 1,
          }],
        }));
      }, 1000);
      return;
    }

    // CONNECT frames are deliberately not answered, as on a real receiver.
  });

  setTimeout(() => {
    console.log(`MOCK SHUTDOWN pongs=${pongs} launches=${launches} loads=${loads}`);
    if (statusTimer) clearInterval(statusTimer);
    server.close();
  }, args.seconds * 1000);
}

main();
