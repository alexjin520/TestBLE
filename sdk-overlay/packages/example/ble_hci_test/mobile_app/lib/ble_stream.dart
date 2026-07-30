import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_connect.dart';
import 'ble_time_sync.dart';
import 'ble_transfer.dart';
import 'protocol.dart';

typedef LogFn = void Function(String msg);
typedef StreamPacketFn = void Function(StreamDataPacket packet);
typedef AuxNotifyFn = void Function(List<int> packet);

/// Board -> phone live sample stream (Notify 0x32).
class BleStreamService {
  StreamSubscription<List<int>>? _notifySub;
  BluetoothCharacteristic? _char;
  BluetoothDevice? _device;
  var _running = false;
  var _packets = 0;
  var _totalSamples = 0;
  var _lostSamples = 0;
  var _lostPackets = 0;
  var _outOfOrderPackets = 0;
  int? _lastSeq;
  int? _boardTotalSamples;
  int? _expectedSampleBase;
  int? _expectedSeq;
  DateTime? _lastLossLogAt;
  String? lastRecordingBasename;
  Completer<String?>? _lastNameWaiter;
  AuxNotifyFn? _auxNotify;

  bool get isRunning => _running;
  bool get hasOpenLink => _char != null && _device != null;
  BluetoothDevice? get linkedDevice => _device;
  BluetoothCharacteristic? get linkedChar => _char;
  int get packetCount => _packets;
  int get totalSamples => _totalSamples;
  int get lostSamples => _lostSamples;
  int get lostPackets => _lostPackets;
  int get outOfOrderPackets => _outOfOrderPackets;

  void bindAuxNotify(AuxNotifyFn? handler) {
    _auxNotify = handler;
  }

  /// BLE STREAM_DATA continuity check (seq + sampleBase; protocol has no CRC).
  StreamFrameCheck get frameCheck => StreamFrameCheck(
        packets: _packets,
        samples: _totalSamples,
        lostPackets: _lostPackets,
        lostSamples: _lostSamples,
        outOfOrderPackets: _outOfOrderPackets,
        lastSeq: _lastSeq,
        boardTotalSamples: _boardTotalSamples,
      );

  void _resetStreamStats() {
    _packets = 0;
    _totalSamples = 0;
    _lostSamples = 0;
    _lostPackets = 0;
    _outOfOrderPackets = 0;
    _lastSeq = null;
    _boardTotalSamples = null;
    _expectedSampleBase = null;
    _expectedSeq = null;
    _lastLossLogAt = null;
  }

  void _trackStreamPacket(StreamDataPacket pkt, LogFn log) {
    if (_expectedSeq != null && pkt.seq > _expectedSeq!) {
      final gap = pkt.seq - _expectedSeq!;
      _lostPackets += gap;
      _logLossThrottled(
        log,
        'STREAM 包序号跳号: 期望 seq=$_expectedSeq got=${pkt.seq} (缺 $gap 包)',
      );
    } else if (_expectedSeq != null && pkt.seq < _expectedSeq!) {
      _outOfOrderPackets++;
      _logLossThrottled(
        log,
        'STREAM 重复/乱序包: seq=${pkt.seq} (期望 $_expectedSeq)',
      );
    }
    _lastSeq = pkt.seq;
    _expectedSeq = pkt.seq + 1;

    if (_expectedSampleBase == null) {
      if (pkt.sampleBase > 0) {
        _lostSamples += pkt.sampleBase;
        _logLossThrottled(
          log,
          'STREAM 首包 sampleBase=${pkt.sampleBase} (缺 ${pkt.sampleBase} 点)',
        );
      }
      _expectedSampleBase = pkt.sampleBase + pkt.samples.length;
    } else if (pkt.sampleBase > _expectedSampleBase!) {
      final gap = pkt.sampleBase - _expectedSampleBase!;
      _lostSamples += gap;
      _logLossThrottled(
        log,
        'STREAM 采样跳号: 期望 index=$_expectedSampleBase got=${pkt.sampleBase} '
        '(缺 $gap 点)',
      );
      _expectedSampleBase = pkt.sampleBase + pkt.samples.length;
    } else if (pkt.sampleBase < _expectedSampleBase!) {
      final overlap = _expectedSampleBase! - pkt.sampleBase;
      if (overlap < pkt.samples.length) {
        _expectedSampleBase = pkt.sampleBase + pkt.samples.length;
      }
    } else {
      _expectedSampleBase = _expectedSampleBase! + pkt.samples.length;
    }
  }

  void _logLossThrottled(LogFn log, String msg) {
    final now = DateTime.now();
    if (_lastLossLogAt == null ||
        now.difference(_lastLossLogAt!) > const Duration(seconds: 2)) {
      _lastLossLogAt = now;
      log(msg);
    }
  }

  String _lossSummary({int? boardTotal}) {
    final buf = StringBuffer();
    if (_lostSamples > 0 || _lostPackets > 0) {
      buf.write(' · 丢 ${_lostSamples}点/${_lostPackets}包');
    } else {
      buf.write(' · 无丢包');
    }
    if (boardTotal != null && _expectedSampleBase != null) {
      final phoneContiguous = _expectedSampleBase!;
      if (boardTotal > phoneContiguous) {
        final tail = boardTotal - phoneContiguous;
        buf.write(' · 尾缺$tail点');
      }
    }
    return buf.toString();
  }

  void _onNotifyData(
    List<int> data,
    LogFn log,
    StreamPacketFn onPacket,
    void Function(int totalSamples)? onStopped,
  ) {
    if (data.isEmpty) return;
    final op = data[0];

    if (op == BleProtocol.opTxLastRsp) {
      final name = BleProtocol.parseTxLastRsp(data);
      log('FILE_TX_LAST_RSP: ${name ?? "(空)"}');
      if (name != null && name.isNotEmpty) {
        lastRecordingBasename = name;
      }
      final waiter = _lastNameWaiter;
      if (waiter != null && !waiter.isCompleted) {
        waiter.complete(name);
      }
      return;
    }

    if (op >= BleProtocol.opLiveStart && op <= BleProtocol.opLiveAbort) {
      _auxNotify?.call(data);
      return;
    }

    if (op == BleProtocol.opFnirsRsp) {
      return;
    }

    if (!_running) return;

    if (op == BleProtocol.opStreamData) {
      final pkt = BleProtocol.parseStreamData(data);
      if (pkt == null) return;
      _trackStreamPacket(pkt, log);
      _packets++;
      _totalSamples += pkt.samples.length;
      onPacket(pkt);
      return;
    }
    if (op == BleProtocol.opStreamStop) {
      final total = BleProtocol.parseStreamStopTotal(data);
      if (total != null) _boardTotalSamples = total;
      log(
        'STREAM STOP 板子总采样=${total ?? _totalSamples}'
        '${_lossSummary(boardTotal: total)}',
      );
      if (total != null) _totalSamples = total;
      onStopped?.call(_totalSamples);
    }
  }

  Future<void> start({
    required BluetoothDevice device,
    required StreamPacketFn onPacket,
    LogFn? onLog,
    void Function(int totalSamples)? onStopped,
    Future<void> Function()? onBeforeStreamStart,
    int rateHz = BleProtocol.defaultStreamRateHz,
    int periodMs = BleProtocol.defaultStreamPeriodMs,
  }) async {
    final log = onLog ?? (_) {};
    if (_running) {
      await stop(onLog: log);
    }

    Object? lastErr;
    for (var attempt = 1; attempt <= 3; attempt++) {
      try {
        if (attempt > 1) {
          log('STREAM 启动重试 $attempt/3…');
          await Future<void>.delayed(Duration(seconds: attempt));
        }
        await _startOnce(
          device: device,
          onPacket: onPacket,
          onStopped: onStopped,
          onBeforeStreamStart: onBeforeStreamStart,
          rateHz: rateHz,
          periodMs: periodMs,
          log: log,
          forceFreshGatt: attempt > 1,
        );
        return;
      } catch (e) {
        lastErr = e;
        if (attempt >= 3 ||
            (!BleTransferService.isGattBusy(e) &&
                !BleConnect.isDisconnectError(e) &&
                !e.toString().contains('Timed out') &&
                !e.toString().contains('扫描未见到') &&
                !e.toString().contains('GATT 预热'))) {
          rethrow;
        }
        log('STREAM GATT 异常: $e');
        await detachNotifyForTransfer(log);
        if (!await BleConnect.isConnected(device)) {
          BleTransferService.clearGattCache();
        }
        BleTransferService.resetGattChain();
        await BleTransferService.drainGattChain();
      }
    }
    throw StateError('$lastErr');
  }

  Future<void> _startOnce({
    required BluetoothDevice device,
    required StreamPacketFn onPacket,
    void Function(int totalSamples)? onStopped,
    Future<void> Function()? onBeforeStreamStart,
    required int rateHz,
    required int periodMs,
    required LogFn log,
    bool forceFreshGatt = false,
  }) async {
    await detachNotifyForTransfer(log);
    BleTransferService.resetGattChain();
    await BleTransferService.drainGattChain();
    if (Platform.isAndroid) {
      try {
        await device.clearGattCache();
      } catch (_) {}
      BleTransferService.clearGattCache();
    }
    if (forceFreshGatt && !await BleConnect.isConnected(device)) {
      BleTransferService.clearGattCache();
    }

    final remoteId = device.remoteId.str;
    final fast = !forceFreshGatt ? await _tryFastStreamLink(device, log) : null;

    final BluetoothDevice connected;
    final BluetoothCharacteristic char;

    if (fast != null) {
      connected = fast.$1;
      char = fast.$2;
      if (!BleTransferService.isWritableChar(char) &&
          !BleTransferService.isXferCharUuid(char)) {
        log('快速路径特征 UUID 不对，重新 discover…');
        _char = null;
        _device = null;
        BleTransferService.clearGattCache();
        return _startOnce(
          device: device,
          onPacket: onPacket,
          onStopped: onStopped,
          onBeforeStreamStart: onBeforeStreamStart,
          rateHz: rateHz,
          periodMs: periodMs,
          log: log,
          forceFreshGatt: true,
        );
      }
    } else {
      final hasCache = BleTransferService.hasCachedGatt(remoteId);
      final stillConnected = await BleConnect.isConnected(device);

      connected = await BleConnect.connectForTransfer(
        remoteId: remoteId,
        preferredDevice: device,
        log: log,
        reuseIfConnected: (hasCache || stillConnected) && !forceFreshGatt,
        forceReconnect: forceFreshGatt,
        disconnectAllFirst: false,
      );

      await connected.connectionState
          .firstWhere((s) => s == BluetoothConnectionState.connected)
          .timeout(const Duration(seconds: 10));
      await BleConnect.waitGattStable(connected, log);
      await Future<void>.delayed(
        Duration(milliseconds: Platform.isAndroid ? 900 : 500),
      );

      final cached =
          !forceFreshGatt ? BleTransferService.cachedGatt(remoteId) : null;

      if (cached != null) {
        log('复用已缓存 GATT 表，跳过 discoverServices');
        char = cached.char;
      } else {
        final services = await BleTransferService.discoverServicesWithRetry(
          connected,
          log,
          clearCacheFirst: forceFreshGatt,
          remoteId: remoteId,
          preferredDevice: connected,
        );
        final foundChar = _findChar(services);
        if (foundChar == null) {
          throw StateError('未找到传输特征 ${BleProtocol.chrUuid}');
        }
        char = foundChar;
        BleTransferService.rememberGatt(remoteId, services, char);
        log('GATT 服务已缓存');
      }
    }

    BleTransferService.logCharProps(char, log);
    if (!BleTransferService.isWritableChar(char)) {
      if (BleTransferService.isXferCharUuid(char)) {
        log('Android 未报告 write 属性，仍向板子特征尝试写入…');
      } else {
        throw StateError(
          '未找到可写传输特征 ${BleProtocol.chrUuid}。'
          '请关开蓝牙后重新扫描连接。',
        );
      }
    }

    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 500 : 250),
    );

    await BleTransferService.gattRun(
      () => BleTransferService.negotiateMtuForDevice(
        connected,
        log,
        trustPhoneMtu: true,
      ),
    );
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 400 : 200),
    );
    log('STREAM MTU=${connected.mtuNow}');

    if (!char.properties.notify && !BleTransferService.isXferCharUuid(char)) {
      throw StateError('板子特征不支持 Notify');
    }
    if (!char.properties.notify) {
      log('警告: 特征未报告 notify，仍尝试订阅 CCCD…');
    }

    await BleTransferService.resetBoardProtocol(char, log);
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 400 : 200),
    );
    await BleTimeSync.pushToCharacteristic(char, log);
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 350 : 200),
    );

    _resetStreamStats();
    lastRecordingBasename = null;
    _lastNameWaiter = null;
    _device = connected;
    _char = char;

    _notifySub = char.onValueReceived.listen(
      (data) => _onNotifyData(data, log, onPacket, onStopped),
    );
    await BleTransferService.prepareStreamNotify(char, log);
    await BleConnect.requestHighPriorityIfAndroid(connected, log);

    if (onBeforeStreamStart != null) {
      log('配置 fNIRS 参数…');
      await onBeforeStreamStart();
    }

    _running = true;

    log('发送 STREAM START rate=$rateHz period=${periodMs}ms');
    await BleTransferService.writeProtocol(
      char,
      BleProtocol.buildStreamStart(rateHz: rateHz, periodMs: periodMs),
      log,
      label: 'STREAM START',
    );
    log('实时流已启动，等待波形…');
  }

  Future<void> stop({
    LogFn? onLog,
    bool keepLink = true,
    bool prefetchLast = true,
  }) async {
    final log = onLog ?? (_) {};
    if (!_running && !keepLink) {
      await _notifySub?.cancel();
      _notifySub = null;
      _char = null;
      _device = null;
      return;
    }
    if (!_running) return;

    _running = false;
    final ch = _char;
    if (ch != null) {
      try {
        await BleTransferService.writeProtocol(
          ch,
          BleProtocol.buildStreamAbort(),
          log,
          label: 'STREAM ABORT',
        );
      } catch (e) {
        log('STREAM ABORT 失败: $e');
      }
    }
    if (!keepLink) {
      await _notifySub?.cancel();
      _notifySub = null;
      _char = null;
      _device = null;
      _lastNameWaiter = null;
    } else if (ch != null && prefetchLast) {
      await Future<void>.delayed(const Duration(milliseconds: 450));
      await prefetchLastRecordingName(log);
    }
    log(
      keepLink
          ? '实时流已停止，连接保持 (packets=$_packets samples=$_totalSamples)'
          : '实时流已停止 (packets=$_packets samples=$_totalSamples)',
    );
  }

  /// Returns false and clears cached char if the BLE link is gone (e.g. board reboot).
  Future<bool> ensureLinkAlive(LogFn log) async {
    final d = _device;
    if (d == null || _char == null) return false;
    if (await BleConnect.isConnected(d)) return true;
    log('实时流连接已失效（板子可能已重启），清除缓存');
    await stop(onLog: log, keepLink: false);
    BleTransferService.clearGattCache();
    return false;
  }

  /// After stream stop, ask board for ble_tx/.last (uses the open notify listener).
  Future<String?> prefetchLastRecordingName(LogFn log) async {
    final name = await fetchLastRecordingName(log);
    if (name != null && name.isNotEmpty) {
      log('板子最近录制已缓存: $name');
    }
    return name;
  }

  /// Query ble_tx/.last on the link left open after [stop(keepLink: true)].
  Future<String?> fetchLastRecordingName(LogFn log) async {
    final ch = _char;
    if (ch == null) return null;
    if (_lastNameWaiter != null) {
      return _lastNameWaiter!.future.timeout(const Duration(seconds: 12));
    }

    lastRecordingBasename = null;
    _lastNameWaiter = Completer<String?>();
    try {
      log('查询最近录制 (FILE_TX_LAST)…');
      await BleTransferService.writeProtocol(
        ch,
        BleProtocol.buildTxLastReq(),
        log,
        label: 'FILE_TX_LAST',
      );
      final name = await _lastNameWaiter!.future.timeout(
        const Duration(seconds: 12),
      );
      if (name != null && name.isNotEmpty) {
        lastRecordingBasename = name;
      }
      return name;
    } on TimeoutException {
      log('查询最近录制超时');
      return null;
    } finally {
      _lastNameWaiter = null;
    }
  }

  /// Re-bind cached GATT after FILE TX without tearing down discover cache.
  void bindCachedLink(
    BluetoothDevice device,
    BluetoothCharacteristic char,
    LogFn log,
  ) {
    _notifySub?.cancel();
    _notifySub = null;
    _device = device;
    _char = char;
    _running = false;
    lastRecordingBasename = null;
    _lastNameWaiter = null;
    log('已绑定 GATT（可再次开始 STREAM）');
  }

  /// Skip connect/discover when link + char already bound (e.g. after download).
  Future<(BluetoothDevice, BluetoothCharacteristic)?> _tryFastStreamLink(
    BluetoothDevice device,
    LogFn log,
  ) async {
    final remoteId = device.remoteId.str;
    if (_char != null &&
        _device != null &&
        _device!.remoteId.str == remoteId &&
        await BleConnect.isConnected(_device!) &&
        (BleTransferService.isWritableChar(_char!) ||
            BleTransferService.isXferCharUuid(_char!))) {
      log('STREAM 快速路径：已绑定特征，跳过 connect/discover');
      return (_device!, _char!);
    }
    if (BleTransferService.hasCachedGatt(remoteId) &&
        await BleConnect.isConnected(device)) {
      final cached = BleTransferService.cachedGatt(remoteId)!;
      if (BleTransferService.isWritableChar(cached.char) ||
          BleTransferService.isXferCharUuid(cached.char)) {
        log('STREAM 快速路径：GATT 缓存 + 已连接，跳过 connect/discover');
        return (device, cached.char);
      }
      log('STREAM 快速路径：缓存特征 UUID 不对，改走完整 discover');
    }
    return null;
  }

  /// Drop cached stream link after FILE TX so the next STREAM re-subscribes cleanly.
  void releaseLinkAfterTransfer() {
    _notifySub?.cancel();
    _notifySub = null;
    _char = null;
    _device = null;
    _running = false;
    _lastNameWaiter = null;
  }

  /// Cancel stream notify listener so FILE TX can own the characteristic.
  Future<void> detachNotifyForTransfer(LogFn log) async {
    if (_notifySub != null) {
      await _notifySub!.cancel();
      _notifySub = null;
      log('已释放实时流 Notify，供 FILE TX 使用');
    }
  }

  Future<void> dispose({LogFn? onLog}) async {
    await stop(onLog: onLog, keepLink: false);
    _auxNotify = null;
  }

  BluetoothCharacteristic? _findChar(List<BluetoothService> services) {
    return BleTransferService.findXferChar(services) ??
        _findCharByUuidOnly(services);
  }

  BluetoothCharacteristic? _findCharByUuidOnly(List<BluetoothService> services) {
    final target = BleProtocol.chrUuid.toLowerCase();
    for (final svc in services) {
      for (final c in svc.characteristics) {
        if (c.uuid.str.toLowerCase() == target) return c;
      }
    }
    return null;
  }

  Future<void> _write(
    BluetoothCharacteristic char,
    Uint8List data,
    LogFn log,
  ) async {
    await BleTransferService.writeProtocol(char, data, log);
  }
}
