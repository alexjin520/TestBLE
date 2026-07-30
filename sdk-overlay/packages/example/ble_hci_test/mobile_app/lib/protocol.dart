import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

/// Same protocol as ble_file_tx.py / mybtgatt-server.c (rev 20250703-v7-fc-fix).
class BleProtocol {
  static const appRev = '20260728-v79-live-tail-retry';
  static const svcUuid = '78563412-3412-7856-1234-567812345678';
  static const chrUuid = '79563412-3412-7856-1234-567812345678';
  static const defaultName = 'TestBLE';
  /// AIC8800 on x2600halley7 boards (btmgmt name still TestBLE).
  static const boardMacPrefixes = ['DC:84:03'];
  /// Example only — replace it with the target board's address when needed.
  static const defaultBoardMac = 'AA:BB:CC:DD:EE:FF';
  static const bleRxDir = '/app_data/ble_rx';
  static const bleTxDir = '/app_data/ble_tx';

  static const opStart = 0x01;
  static const opData = 0x02;
  static const opEnd = 0x03;
  static const opAbort = 0x11;

  static const opTxReq = 0x21;
  static const opTxStart = 0x22;
  static const opTxData = 0x23;
  static const opTxEnd = 0x24;
  static const opTxAbort = 0x25;
  static const opTxAck = 0x26;
  static const opTxLastReq = 0x27;
  static const opTxLastRsp = 0x28;

  static const opLiveReq = 0x29;
  static const opLiveStart = 0x2a;
  static const opLiveData = 0x2b;
  static const opLiveFinal = 0x2c;
  static const opLiveAck = 0x2d;
  static const opLiveAbort = 0x2e;
  static const liveFinalSealed = 0;
  static const liveFinalDone = 1;

  static const opStreamStart = 0x31;
  static const opStreamData = 0x32;
  static const opStreamStop = 0x33;
  static const opStreamAbort = 0x34;
  static const opTimeSet = 0x35;

  static const opFnirsScan = 0x40;
  static const opFnirsSampleOn = 0x41;
  static const opFnirsSampleOff = 0x42;
  static const opFnirsGain = 0x43;
  static const opFnirsLedArray = 0x44;
  static const opFnirsStreamCh = 0x45;
  static const opFnirsHangzhou = 0x46;
  static const opFnirsRecordStat = 0x47;
  static const opFnirsRsp = 0x48;

  static const fnirsStatusOk = 0;
  static const defaultStreamRateHz = 80;
  static const defaultStreamPeriodMs = 10;
  /// Target effective sample payload: rateHz * 2 bytes/s (~10240 B/s ≈ 10 KB/s).
  /// Use period>=12ms so Notify pkt rate (~83/s) fits BLE conn interval (~7.5ms).
  static const streamTargetBytesPerSec = 12000;
  //static const streamTargetBytesPerSec = 10240;

  /// Board->phone download ACK window (must match FILE_TX_FC_WINDOW in mybtgatt-server.c).
  static const txFcWindowPkts = 14;
  static const txFcWaitStepPkts = 2;

  static const attWriteOverhead = 3;
  static const dataHdrLen = 7;
  static const dataCrcLen = 2;

  static const statusMagic = 0xA5;
  static const rxStateIdle = 0x00;
  static const rxStateActive = 0x01;
  static const rxStateDone = 0x02;
  static const rxStateError = 0x03;

  static const rxErrFrameCrc = 0xE1;
  static const rxErrSeq = 0xE2;
  static const rxErrFrameLen = 0xE3;

  static const defaultMtuRequest = 247;
  static const safeChunk = 128;
  static const safeDelayMs = 10;
  /// Board raw ATT is 23 until the link proves a larger MTU (PC Bleak may get 247).
  static const boardAttMtuCap = 23;
  static const safeChunkCap = 200;
  static const fastChunkDefault = 200;
  static const largeFileChunk = fastChunkDefault;

  /// Must stay under mybtgatt-server FILE_RX_IO_BUF_SIZE (64KB) headroom.
  static const boardRxBufBytes = 56 * 1024;
  static const defaultFcWindowPkts = 64;
  static const fastFcWindowPkts = 96;
  static const defaultFcWaitStepPkts = 1;
  static const fastFcWaitStepPkts = 4;

  static const flowControlTimeoutSec = 20.0;
  static const firstStatusTimeoutSec = 8.0;
  static const flowControlPollMs = 50;
  static const notifySettleMs = 350;
  static const startupExtraDelayMs = 200;

  static const defaultTxDelayMs = 2;
  static const txStartTimeoutSec = 15.0;
  static const txPacketTimeoutSec = 45.0;
  static const defaultSimFilename = 'sim_signal.bin';
  static const simMagic = 'fSIM';
  static const simHeaderLen = 24;
  static const simVersion = 1;
  static const legacyFallbackTxDelayMs = 6;
  static const warmupPackets = 6;
  static const warmupDelayMs = 4;
  static const resyncIntervalPkts = 32;
  static const maxTransferAttempts = 5;
  static const progressIntervalPkts = 32;
  static const minTrustedWwrPayload = 64;

  /// Notify sliding-window params (same as get_fc_params in ble_file_tx.py).
  static ({int window, int step}) fcParams({required bool fast}) {
    return (
      window: fast ? fastFcWindowPkts : defaultFcWindowPkts,
      step: fast ? fastFcWaitStepPkts : defaultFcWaitStepPkts,
    );
  }

  /// Cap in-flight packets so bytes in flight fit board RX buffer.
  static int clampFcWindow(int windowPkts, int chunkBytes) {
    if (chunkBytes <= 0) return windowPkts;
    final maxPkts = math.max(8, boardRxBufBytes ~/ chunkBytes);
    return math.min(windowPkts, maxPkts);
  }

  /// True when sender should block until board next_seq catches up.
  static bool shouldFcWait(int seq, int fcWindow, int fcWaitStep) {
    if (seq < fcWindow) return false;
    return seq == fcWindow || (seq % fcWaitStep == 0);
  }

  static const rxStateNames = <int, String>{
    rxStateIdle: 'IDLE',
    rxStateActive: 'ACTIVE',
    rxStateDone: 'DONE',
    rxStateError: 'ERROR',
  };

  static const rxErrNames = <int, String>{
    rxErrFrameCrc: 'FRAME_CRC',
    rxErrSeq: 'SEQ',
    rxErrFrameLen: 'FRAME_LEN',
  };

  static const _crc16Table = <int>[
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
  ];

  static int crc32Update(int previous, List<int> data) {
    var crc = (~previous) & 0xFFFFFFFF;
    for (final b in data) {
      crc ^= b;
      for (var i = 0; i < 8; i++) {
        crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
      }
    }
    return (~crc) & 0xFFFFFFFF;
  }

  static int crc32Ieee(List<int> data) => crc32Update(0, data);

  static int crc16Fnirs(List<int> data, {int init = 0xFFFF}) {
    var crc = init & 0xFFFF;
    for (final b in data) {
      crc = ((crc << 8) ^ _crc16Table[((crc >> 8) ^ b) & 0xFF]) & 0xFFFF;
    }
    return crc;
  }

  static Uint8List buildStart(String filename, int size, int crc) {
    final name = utf8.encode(filename);
    if (name.length > 127) {
      throw ArgumentError('filename too long');
    }
    final out = BytesBuilder();
    out.addByte(opStart);
    out.addByte(name.length);
    out.add(name);
    final meta = ByteData(8)
      ..setUint32(0, size, Endian.little)
      ..setUint32(4, crc, Endian.little);
    out.add(meta.buffer.asUint8List());
    return out.toBytes();
  }

  static Uint8List buildData(int seq, Uint8List chunk, {required bool frameCrc}) {
    final body = BytesBuilder();
    final hdr = ByteData(6)
      ..setUint32(0, seq, Endian.little)
      ..setUint16(4, chunk.length, Endian.little);
    body.add(hdr.buffer.asUint8List());
    body.add(chunk);
    final bodyBytes = body.toBytes();
    final out = BytesBuilder()
      ..addByte(opData)
      ..add(bodyBytes);
    if (frameCrc) {
      final c = crc16Fnirs(bodyBytes);
      final crcBytes = ByteData(2)..setUint16(0, c, Endian.little);
      out.add(crcBytes.buffer.asUint8List());
    }
    return out.toBytes();
  }

  static Uint8List buildEnd() => Uint8List.fromList([opEnd]);

  static Uint8List buildAbort() => Uint8List.fromList([opAbort]);

  static Uint8List buildTxReq(String filename) {
    final name = utf8.encode(filename);
    if (name.isEmpty || name.length > 127) {
      throw ArgumentError('filename invalid');
    }
    return Uint8List.fromList([opTxReq, name.length, ...name]);
  }

  static Uint8List buildTxAbort() => Uint8List.fromList([opTxAbort]);

  static Uint8List buildTxLastReq() => Uint8List.fromList([opTxLastReq]);

  /// Parse board FILE_TX_LAST_RSP notify: [op][name_len][name].
  static String? parseTxLastRsp(List<int> data) {
    if (data.length < 2 || data[0] != opTxLastRsp) return null;
    final nameLen = data[1];
    if (nameLen == 0 || data.length < nameLen + 2) return null;
    return utf8.decode(data.sublist(2, 2 + nameLen), allowMalformed: true);
  }

  /// Phone -> board: next expected DATA seq (opens the notify window).
  static Uint8List buildTxAck(int nextSeq) {
    final b = ByteData(5)
      ..setUint8(0, opTxAck)
      ..setUint32(1, nextSeq, Endian.little);
    return b.buffer.asUint8List();
  }

  static Uint8List buildLiveReq() => Uint8List.fromList([opLiveReq]);

  static Uint8List buildLiveAck(int nextSeq, int nextOffset) {
    final b = ByteData(9)
      ..setUint8(0, opLiveAck)
      ..setUint32(1, nextSeq, Endian.little)
      ..setUint32(5, nextOffset, Endian.little);
    return b.buffer.asUint8List();
  }

  static Uint8List buildLiveAbort() => Uint8List.fromList([opLiveAbort]);

  static Uint8List buildStreamStart({
    int rateHz = defaultStreamRateHz,
    int periodMs = defaultStreamPeriodMs,
  }) {
    final b = ByteData(7)
      ..setUint8(0, opStreamStart)
      ..setUint32(1, rateHz, Endian.little)
      ..setUint16(5, periodMs, Endian.little);
    return b.buffer.asUint8List();
  }

  static Uint8List buildStreamAbort() => Uint8List.fromList([opStreamAbort]);

  /// Phone -> board: set Unix time (seconds) and write RTC if present.
  static Uint8List buildTimeSet(int unixSec) {
    final b = ByteData(5)
      ..setUint8(0, opTimeSet)
      ..setUint32(1, unixSec, Endian.little);
    return b.buffer.asUint8List();
  }

  static Uint8List buildFnirsScan() => Uint8List.fromList([opFnirsScan]);

  static Uint8List buildFnirsSampleOn() =>
      Uint8List.fromList([opFnirsSampleOn]);

  static Uint8List buildFnirsSampleOff() =>
      Uint8List.fromList([opFnirsSampleOff]);

  static Uint8List buildFnirsGain(int gain) =>
      Uint8List.fromList([opFnirsGain, gain & 0xff]);

  /// Default LED array on board (nodes 10–12), matches sample_node_array.
  static const defaultLedArrayEntries = <List<int>>[
    [10, 2, 0x1E, 0x1E],
    [11, 1, 0x1E, 0x1E],
    [11, 2, 0x1E, 0x1E],
    [12, 2, 0x1E, 0x1E],
  ];

  /// SSCom-style test sequence (nodes 1–2, L1/L2).
  static const testLedArrayNode12Entries = <List<int>>[
    [1, 1, 0x28, 0x1A],
    [1, 2, 0x28, 0x1A],
    [2, 1, 0x28, 0x1A],
    [2, 2, 0x28, 0x1A],
  ];

  static const defaultLedPower735 = 0x28;
  static const defaultLedPower850 = 0x1A;

  static List<List<int>> cloneLedArrayEntries(List<List<int>> src) =>
      src.map((e) => List<int>.from(e)).toList();

  static String ledArrayStepLabel(List<int> e) => 'N${e[0]}-L${e[1]}';

  static String ledArraySummary(List<List<int>> entries) {
    if (entries.isEmpty) return '未配置';
    final nodes = entries.map((e) => e[0]).toSet().toList()..sort();
    final period = ledArrayEstimatePeriodMs(entries);
    return '${entries.length} 步 · 源节点 ${nodes.join(', ')} · ~${period}ms/帧';
  }

  static int ledArrayEstimatePeriodMs(List<List<int>> entries) =>
      11 * entries.length;

  static List<List<int>> ledArrayWithUniformPower(
    List<List<int>> entries,
    int power735,
    int power850,
  ) =>
      entries
          .map((e) => [e[0], e[1], power735 & 0xff, power850 & 0xff])
          .toList();

  static Uint8List buildFnirsLedArray(List<List<int>> entries) {
    final out = BytesBuilder();
    out.addByte(opFnirsLedArray);
    for (final e in entries) {
      out.addByte(e[0]);
      out.addByte(e[1]);
      out.addByte(e[2]);
      out.addByte(e[3]);
    }
    return out.toBytes();
  }

  static Uint8List buildDefaultFnirsLedArray() =>
      buildFnirsLedArray(defaultLedArrayEntries);

  static Uint8List buildFnirsStreamCh(List<FnirsChannel> channels) {
    final out = BytesBuilder();
    out.addByte(opFnirsStreamCh);
    out.addByte(channels.length);
    for (final c in channels) {
      out.addByte(c.srcNode);
      out.addByte(c.ledId);
      out.addByte(c.detNode);
      out.addByte(c.detId);
    }
    return out.toBytes();
  }

  static Uint8List buildFnirsHangzhouReq() =>
      Uint8List.fromList([opFnirsHangzhou]);

  static Uint8List buildFnirsRecordStatReq() =>
      Uint8List.fromList([opFnirsRecordStat]);

  static FnirsRsp? parseFnirsRsp(List<int> data) {
    if (data.length < 3 || data[0] != opFnirsRsp) return null;
    final echo = data[1];
    final status = data[2];
    final payload = data.length > 3 ? data.sublist(3) : <int>[];
    return FnirsRsp(echoOp: echo, status: status, payload: payload);
  }

  static List<bool> parseFnirsScanAlive(List<int> payload) {
    final alive = List<bool>.filled(12, false);
    for (var i = 0; i < payload.length && i < 12; i++) {
      alive[i] = payload[i] != 0;
    }
    return alive;
  }

  static String? parseFnirsHangzhouPath(List<int> payload) {
    if (payload.isEmpty) return null;
    final nul = payload.indexOf(0);
    final bytes = nul >= 0 ? payload.sublist(0, nul) : payload;
    if (bytes.isEmpty) return null;
    return utf8.decode(bytes, allowMalformed: true);
  }

  static FnirsRecordStat? parseFnirsRecordStat(List<int> payload) {
    if (payload.length < 8) return null;
    final bd = ByteData.sublistView(Uint8List.fromList(payload));
    final bytes = bd.getUint32(0, Endian.little);
    final frames = bd.getUint32(4, Endian.little);
    return FnirsRecordStat(bytes: bytes, frames: frames);
  }

  /// Parse board STREAM_DATA notify: [op][seq:4][base:4][count:2][int16...]
  static StreamDataPacket? parseStreamData(List<int> data) {
    if (data.length < 11 || data[0] != opStreamData) return null;
    final hdr = ByteData.sublistView(Uint8List.fromList(data), 0, 11);
    final seq = hdr.getUint32(1, Endian.little);
    final sampleBase = hdr.getUint32(5, Endian.little);
    final count = hdr.getUint16(9, Endian.little);
    if (count == 0 || data.length < 11 + count * 2) return null;

    final samples = <int>[];
    for (var i = 0; i < count; i++) {
      final off = 11 + i * 2;
      final v = ByteData.sublistView(Uint8List.fromList(data), off, off + 2)
          .getInt16(0, Endian.little);
      samples.add(v);
    }
    return StreamDataPacket(seq: seq, sampleBase: sampleBase, samples: samples);
  }

  static int? parseStreamStopTotal(List<int> data) {
    if (data.length < 5 || data[0] != opStreamStop) return null;
    return ByteData.sublistView(Uint8List.fromList(data), 1, 5)
        .getUint32(0, Endian.little);
  }

  /// Build fSIM file from captured int16 samples (live stream save).
  static Uint8List buildSimSignalFile(
    List<int> int16Samples, {
    int sampleRateHz = defaultStreamRateHz,
    int channels = 1,
    int bitsPerSample = 16,
  }) {
    if (int16Samples.isEmpty) {
      throw ArgumentError('no samples');
    }
    final hdr = ByteData(simHeaderLen)
      ..setUint8(0, 0x66)
      ..setUint8(1, 0x53)
      ..setUint8(2, 0x49)
      ..setUint8(3, 0x4d)
      ..setUint16(4, simVersion, Endian.little)
      ..setUint16(6, 0, Endian.little)
      ..setUint32(8, sampleRateHz, Endian.little)
      ..setUint32(12, int16Samples.length, Endian.little)
      ..setUint16(16, channels, Endian.little)
      ..setUint16(18, bitsPerSample, Endian.little)
      ..setUint32(20, 0, Endian.little);
    final out = BytesBuilder(copy: false)..add(hdr.buffer.asUint8List());
    for (final s in int16Samples) {
      final b = ByteData(2)..setInt16(0, s, Endian.little);
      out.add(b.buffer.asUint8List());
    }
    return out.toBytes();
  }

  /// Parse board FILE_TX_START notify (same layout as START).
  static TxStartInfo? parseTxStart(List<int> data) {
    if (data.length < 10 || data[0] != opTxStart) return null;
    final nameLen = data[1];
    if (data.length < nameLen + 10) return null;
    final name = utf8.decode(data.sublist(2, 2 + nameLen), allowMalformed: true);
    final meta = ByteData.sublistView(Uint8List.fromList(data), 2 + nameLen, 2 + nameLen + 8);
    return TxStartInfo(
      filename: name,
      size: meta.getUint32(0, Endian.little),
      crc32: meta.getUint32(4, Endian.little),
    );
  }

  /// Parse board FILE_TX_DATA notify. Returns null on malformed packet.
  static TxDataFrame? parseTxData(List<int> data, {required bool expectFrameCrc}) {
    if (data.length < 7 || data[0] != opTxData) return null;
    final seq = ByteData.sublistView(Uint8List.fromList(data), 1, 5)
        .getUint32(0, Endian.little);
    final declLen = ByteData.sublistView(Uint8List.fromList(data), 5, 7)
        .getUint16(0, Endian.little);

    if (expectFrameCrc && data.length >= 7 + declLen + dataCrcLen) {
      final payloadLen = data.length - 7 - dataCrcLen;
      if (declLen == payloadLen) {
        final body = data.sublist(1, 7 + payloadLen);
        final rxCrc = ByteData.sublistView(Uint8List.fromList(data), 7 + payloadLen, 7 + payloadLen + 2)
            .getUint16(0, Endian.little);
        final calc = crc16Fnirs(body);
        if (rxCrc == calc) {
          return TxDataFrame(seq: seq, chunk: Uint8List.fromList(data.sublist(7, 7 + payloadLen)));
        }
        return null;
      }
    }

    // Board always appends frame CRC; accept it even when the UI toggle is off.
    if (!expectFrameCrc && data.length >= 7 + declLen + dataCrcLen) {
      return TxDataFrame(
        seq: seq,
        chunk: Uint8List.fromList(data.sublist(7, 7 + declLen)),
      );
    }

    final chunkLen = data.length - 7;
    if (declLen != chunkLen) return null;
    return TxDataFrame(seq: seq, chunk: Uint8List.fromList(data.sublist(7)));
  }

  /// LIVE START: [op][name_len][name]. Final size/CRC arrive after SAMPLE_OFF.
  static LiveStartInfo? parseLiveStart(List<int> data) {
    if (data.length < 3 || data[0] != opLiveStart) return null;
    final nameLen = data[1];
    if (nameLen == 0 || data.length < nameLen + 2) return null;
    return LiveStartInfo(
      filename: utf8.decode(
        data.sublist(2, 2 + nameLen),
        allowMalformed: true,
      ),
    );
  }

  /// LIVE DATA: [op][seq:4][offset:4][len:2][payload][crc16:2].
  static LiveDataFrame? parseLiveData(List<int> data) {
    if (data.length < 13 || data[0] != opLiveData) return null;
    final bytes = Uint8List.fromList(data);
    final hdr = ByteData.sublistView(bytes, 1, 11);
    final seq = hdr.getUint32(0, Endian.little);
    final offset = hdr.getUint32(4, Endian.little);
    final len = hdr.getUint16(8, Endian.little);
    if (len == 0 || data.length != 13 + len) return null;
    final body = data.sublist(1, 11 + len);
    final rxCrc = ByteData.sublistView(bytes, 11 + len, 13 + len)
        .getUint16(0, Endian.little);
    if (crc16Fnirs(body) != rxCrc) return null;
    return LiveDataFrame(
      seq: seq,
      offset: offset,
      chunk: Uint8List.fromList(data.sublist(11, 11 + len)),
    );
  }

  /// LIVE FINAL: [op][SEALED|DONE][size:4][crc32:4].
  static LiveFinalInfo? parseLiveFinal(List<int> data) {
    if (data.length < 10 || data[0] != opLiveFinal) return null;
    final meta = ByteData.sublistView(Uint8List.fromList(data), 2, 10);
    return LiveFinalInfo(
      done: data[1] == liveFinalDone,
      size: meta.getUint32(0, Endian.little),
      crc32: meta.getUint32(4, Endian.little),
    );
  }

  static int maxAttPayload(int mtu) =>
      (mtu - attWriteOverhead).clamp(1, 512);

  /// START = opcode(1) + name_len(1) + name + size(4) + crc32(4).
  static int maxStartFilenameBytes(int mtu) =>
      math.max(0, maxAttPayload(mtu) - 10);

  /// Shorten filename so START fits in one ATT write (board requires full START).
  static String truncateFilenameForMtu(String filename, int mtu) {
    final maxName = maxStartFilenameBytes(mtu);
    final bytes = utf8.encode(filename);
    if (bytes.length <= maxName) return filename;
    var end = maxName;
    while (end > 0) {
      final candidate = utf8.decode(bytes.sublist(0, end), allowMalformed: true);
      if (utf8.encode(candidate).length <= maxName) return candidate;
      end--;
    }
    return '';
  }

  static int maxChunkForMtu(int mtu, {required bool frameCrc}) {
    final extra = frameCrc ? dataCrcLen : 0;
    return (mtu - attWriteOverhead - dataHdrLen - extra).clamp(1, 512);
  }

  /// Parse full 20-byte board RX status block (same as parse_rx_status_full).
  static RxStatus parseRxStatusFull(List<int> data) {
    int u32(int off) {
      if (data.length < off + 4) return 0;
      return ByteData.sublistView(Uint8List.fromList(data), off, off + 4)
          .getUint32(0, Endian.little);
    }

    return RxStatus(
      magic: data.isNotEmpty ? data[0] : 0,
      state: data.length > 1 ? data[1] : 0,
      error: data.length > 2 ? data[2] : 0,
      received: u32(4),
      expected: u32(8),
      nextSeq: u32(12),
      runningCrc: u32(16),
    );
  }

  static String formatRxStatus(List<int> data) {
    final info = parseRxStatusFull(data);
    final stateName = rxStateNames[info.state] ?? '0x${info.state.toRadixString(16)}';
    final lines = <String>[
      'state:     $stateName',
      'received:  ${info.received} bytes',
      'expected:  ${info.expected} bytes',
      'next_seq:  ${info.nextSeq}',
      'running_crc: 0x${info.runningCrc.toRadixString(16).padLeft(8, '0')}',
    ];
    if (info.error != 0) {
      final errName = rxErrNames[info.error] ?? '0x${info.error.toRadixString(16)}';
      lines.add('last_error: $errName');
    }
    if (info.magic != statusMagic && info.magic != 0) {
      lines.add('magic:      0x${info.magic.toRadixString(16)} (expected 0xA5)');
    }
    return lines.join('\n');
  }
}

class RxStatus {
  const RxStatus({
    required this.magic,
    required this.state,
    required this.error,
    required this.received,
    required this.expected,
    required this.nextSeq,
    required this.runningCrc,
  });

  final int magic;
  final int state;
  final int error;
  final int received;
  final int expected;
  final int nextSeq;
  final int runningCrc;

  bool get isValid => magic == BleProtocol.statusMagic;
  bool get isDone =>
      state == BleProtocol.rxStateDone && received == expected;
  bool get isError => state == BleProtocol.rxStateError;
}

class TxStartInfo {
  const TxStartInfo({
    required this.filename,
    required this.size,
    required this.crc32,
  });

  final String filename;
  final int size;
  final int crc32;
}

class TxDataFrame {
  const TxDataFrame({required this.seq, required this.chunk});

  final int seq;
  final Uint8List chunk;
}

class LiveStartInfo {
  const LiveStartInfo({required this.filename});

  final String filename;
}

class LiveDataFrame {
  const LiveDataFrame({
    required this.seq,
    required this.offset,
    required this.chunk,
  });

  final int seq;
  final int offset;
  final Uint8List chunk;
}

class LiveFinalInfo {
  const LiveFinalInfo({
    required this.done,
    required this.size,
    required this.crc32,
  });

  final bool done;
  final int size;
  final int crc32;
}

/// Phase-2 demo file header (matches ble_sim_signal.py / ble_sim_signal.sh).
class SimSignalInfo {
  const SimSignalInfo({
    required this.version,
    required this.sampleRateHz,
    required this.numSamples,
    required this.channels,
    required this.bitsPerSample,
  });

  final int version;
  final int sampleRateHz;
  final int numSamples;
  final int channels;
  final int bitsPerSample;

  int get bytesPerSample => (bitsPerSample ~/ 8) * channels;
  int get expectedPayloadBytes => numSamples * bytesPerSample;
  int get expectedFileSize => BleProtocol.simHeaderLen + expectedPayloadBytes;

  double get durationSec =>
      sampleRateHz > 0 ? numSamples / sampleRateHz : 0.0;

  String get demoSummary =>
      '模拟 fNIRS · $numSamples 点 · $sampleRateHz Hz · '
      '$channels 通道 · ${durationSec.toStringAsFixed(1)} s';

  static SimSignalInfo? tryParseBytes(List<int> data) {
    if (data.length < BleProtocol.simHeaderLen) return null;
    final magic = utf8.decode(data.sublist(0, 4), allowMalformed: true);
    if (magic != BleProtocol.simMagic) return null;

    final hdr = ByteData.sublistView(Uint8List.fromList(data), 0, BleProtocol.simHeaderLen);
    final version = hdr.getUint16(4, Endian.little);
    final sampleRateHz = hdr.getUint32(8, Endian.little);
    final numSamples = hdr.getUint32(12, Endian.little);
    final channels = hdr.getUint16(16, Endian.little);
    final bitsPerSample = hdr.getUint16(18, Endian.little);

    if (version != BleProtocol.simVersion) return null;
    if (sampleRateHz == 0 || numSamples == 0 || channels == 0 || bitsPerSample == 0) {
      return null;
    }
    if (bitsPerSample % 8 != 0) return null;

    final info = SimSignalInfo(
      version: version,
      sampleRateHz: sampleRateHz,
      numSamples: numSamples,
      channels: channels,
      bitsPerSample: bitsPerSample,
    );
    if (data.length != info.expectedFileSize) return null;
    return info;
  }
}

class StreamDataPacket {
  const StreamDataPacket({
    required this.seq,
    required this.sampleBase,
    required this.samples,
  });

  final int seq;
  final int sampleBase;
  final List<int> samples;
}

/// BLE `STREAM_DATA` (0x32) continuity result — seq + sampleBase (no per-packet CRC).
class StreamFrameCheck {
  const StreamFrameCheck({
    required this.packets,
    required this.samples,
    required this.lostPackets,
    required this.lostSamples,
    required this.outOfOrderPackets,
    this.lastSeq,
    this.boardTotalSamples,
  });

  final int packets;
  final int samples;
  final int lostPackets;
  final int lostSamples;
  final int outOfOrderPackets;
  final int? lastSeq;
  final int? boardTotalSamples;

  bool get hasData => packets > 0;

  int? get tailMissing {
    if (boardTotalSamples == null) return null;
    final gap = boardTotalSamples! - samples;
    return gap > 0 ? gap : 0;
  }

  bool get passed =>
      hasData &&
      lostPackets == 0 &&
      lostSamples == 0 &&
      outOfOrderPackets == 0 &&
      (tailMissing ?? 0) == 0;
}

class FnirsChannel {
  const FnirsChannel({
    required this.srcNode,
    required this.ledId,
    required this.detNode,
    this.detId = 1,
  });

  final int srcNode;
  final int ledId;
  final int detNode;
  final int detId;

  String get label => 'N$srcNode-L$ledId→N$detNode-D$detId';
}

class FnirsRsp {
  const FnirsRsp({
    required this.echoOp,
    required this.status,
    required this.payload,
  });

  final int echoOp;
  final int status;
  final List<int> payload;

  bool get ok => status == BleProtocol.fnirsStatusOk;
}

class FnirsRecordStat {
  const FnirsRecordStat({required this.bytes, required this.frames});

  final int bytes;
  final int frames;
}
