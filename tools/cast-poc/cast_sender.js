// R2 sender: connect to a Cast receiver over TLS:8009, LAUNCH the Default Media
// Receiver, LOAD an HLS URL, and record MEDIA_STATUS as JSONL for judge_r1.py.
//
// This is the POC counterpart of the C++ sender we will eventually write. It
// deliberately shows the CastV2 envelope fields that matter as much as payload:
// sourceId / destinationId / namespace / requestId. node-castv2's Client may set
// rejectUnauthorized=false for us (R2 assumption), but we pass it explicitly so
// the behaviour is visible and does not depend on library internals.
//
// Usage:
//   npm i castv2@0.1.10
//   node cast_sender.js --host 192.168.1.50 --url http://192.168.1.10:8080/live.m3u8
//   node cast_sender.js --host 127.0.0.1 --url ... --seconds 75 --out trace.jsonl
//
// Exit: 0 normal end of observation, 1 connection/protocol error.

'use strict';

const fs = require('fs');
const path = require('path');
const Client = require('castv2').Client;

const APP_ID = 'CC1AD845'; // Default Media Receiver
const SENDER_ID = 'sender-0';
const RECEIVER_ID = 'receiver-0';
const NS_CONNECTION = 'urn:x-cast:com.google.cast.tp.connection';
const NS_RECEIVER = 'urn:x-cast:com.google.cast.receiver';
const NS_HEARTBEAT = 'urn:x-cast:com.google.cast.tp.heartbeat';
const NS_MEDIA = 'urn:x-cast:com.google.cast.media';
const DEFAULT_RECEIVER_SECONDS = 75;

function parseArgs(argv) {
  const args = {
    host: null,
    port: 8009,
    url: null,
    seconds: DEFAULT_RECEIVER_SECONDS,
    out: 'trace.jsonl',
  };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    const value = argv[++i];
    if (key === '--host') args.host = value;
    else if (key === '--port') args.port = Number(value);
    else if (key === '--url') args.url = value;
    else if (key === '--seconds') args.seconds = Number(value);
    else if (key === '--out') args.out = value;
    else if (key === '--help') {
      console.log('Usage: node cast_sender.js --host <ip> --url <m3u8> [--port 8009] [--seconds 75] [--out trace.jsonl]');
      process.exit(0);
    }
  }
  return args;
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.host || !args.url) {
    console.error('ERROR --host and --url are required');
    process.exit(2);
  }
  const tracePath = path.resolve(args.out);
  const startedAt = Date.now();
  const trace = [];

  // JSONL writer: one object per line, timestamps relative to process start.
  function record(event, fields) {
    trace.push(Object.assign({ t_ms: Date.now() - startedAt, event }, fields));
    fs.writeFileSync(tracePath, trace.map((row) => JSON.stringify(row)).join('\n') + '\n', 'utf8');
  }

  const client = new Client();
  let requestId = 0;
  const nextRequestId = () => ++requestId;
  let mediaChannel = null;
  let heartbeat = null;
  let watchdog = null;
  let firstStatusLogged = false;

  function fail(message, err) {
    record('error', { reason: message, detail: err ? String(err.message || err) : '' });
    console.error('ERROR', message, err || '');
    cleanup();
    process.exitCode = 1;
  }

  function cleanup() {
    if (heartbeat) clearInterval(heartbeat);
    if (watchdog) clearTimeout(watchdog);
    if (mediaChannel) mediaChannel.close();
    try { client.close(); } catch (_) { /* socket may already be gone */ }
  }

  client.on('error', (err) => fail('client_error', err));

  client.connect({ host: args.host, port: args.port, rejectUnauthorized: false }, () => {
    console.log(`connected to ${args.host}:${args.port}`);

    const connection = client.createChannel(SENDER_ID, RECEIVER_ID, NS_CONNECTION, 'JSON');
    const receiver = client.createChannel(SENDER_ID, RECEIVER_ID, NS_RECEIVER, 'JSON');
    const heartbeatChannel = client.createChannel(SENDER_ID, RECEIVER_ID, NS_HEARTBEAT, 'JSON');

    connection.send({ type: 'CONNECT' });

    // Heartbeat must run on the device channel for the whole session. Stopping
    // it is a classic reason a session silently dies mid-playback.
    heartbeat = setInterval(() => {
      try { heartbeatChannel.send({ type: 'PING' }); } catch (_) { /* cleaned up on close */ }
    }, 5000);

    receiver.on('message', (data) => {
      if (data.requestId === undefined) return;
      if (data.type === 'RECEIVER_STATUS' && data.status && data.status.applications) {
        const app = data.status.applications.find((a) => a.appId === APP_ID);
        if (app && app.transportId && !mediaChannel) {
          openMediaChannel(app.transportId);
        }
      }
      if (data.type === 'LAUNCH_ERROR') {
        fail('launch_error', new Error(JSON.stringify(data)));
      }
    });

    receiver.send({ type: 'LAUNCH', appId: APP_ID, requestId: nextRequestId() });
    watchdog = setTimeout(() => {
      if (!mediaChannel) fail('launch_timeout_no_transport_id');
    }, 10000);
  });

  function openMediaChannel(transportId) {
    if (watchdog) { clearTimeout(watchdog); watchdog = null; }
    console.log(`launched ${APP_ID}, transportId=${transportId}`);

    mediaChannel = client.createChannel(SENDER_ID, transportId, NS_MEDIA, 'JSON');
    const mediaConnection = client.createChannel(SENDER_ID, transportId, NS_CONNECTION, 'JSON');
    mediaConnection.send({ type: 'CONNECT' });

    mediaChannel.on('message', (data) => {
      if (data.type === 'MEDIA_STATUS' && Array.isArray(data.status)) {
        for (const status of data.status) {
          record('status', {
            playerState: status.playerState,
            currentTime: status.currentTime,
            idleReason: status.idleReason,
            statusResult: status.statusResult,
            sessionId: status.mediaSessionId,
          });
          if (!firstStatusLogged) {
            firstStatusLogged = true;
            console.log(`first MEDIA_STATUS state=${status.playerState} t=${status.currentTime}`);
          }
        }
      } else if (data.type === 'LOAD_FAILED') {
        record('error', { reason: 'LOAD_FAILED', detail: JSON.stringify(data) });
      }
    });

    // LOAD with an HLS URL. streamType=LIVE is required for the CAF receiver to
    // treat the sliding playlist as live rather than a finite VOD playlist.
    record('load', { url: args.url, contentType: 'application/x-mpegURL', streamType: 'LIVE' });
    mediaChannel.send({
      type: 'LOAD',
      requestId: nextRequestId(),
      media: {
        contentId: args.url,
        contentType: 'application/x-mpegURL',
        streamType: 'LIVE',
      },
      autoplay: true,
      currentTime: 0,
    });

    // Observation window: judge_r1.py needs a full 60 s after the first frame.
    setTimeout(() => {
      console.log(`observation complete (${args.seconds}s), wrote ${trace.length} events to ${tracePath}`);
      cleanup();
      process.exit(0);
    }, args.seconds * 1000);
  }
}

main();
