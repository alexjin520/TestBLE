import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'protocol.dart';
import 'ble_connect.dart';

typedef LogFn = void Function(String msg);
typedef ProgressFn = void Function(int sent, int total, double bytesPerSec);

/// App-side pause / cancel. Does not change board protocol.
class TransferController {
  bool _paused = false;
  bool _cancelled = false;

  bool get isPaused => _paused;
  bool get isCancelled => _cancelled;

  void pause() => _paused = true;

  void resume() => _paused = false;

  void cancel() {
    _cancelled = true;
    _paused = false;
  }

  Future<void> waitIfPaused() async {
    while (_paused && !_cancelled) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    if (_cancelled) throw TransferCancelledException();
  }
}

class TransferCancelledException implements Exception {
  @override
  String toString() => '传输已取消';
}

/// Collect latest 20-byte board status from Notify (magic 0xA5 only).
class StatusWatcher {
  Uint8List? latest;
  final _event = StreamController<void>.broadcast();

  void onData(List<int> data) {
    if (data.length >= 20 && data[0] == BleProtocol.statusMagic) {
      latest = Uint8List.fromList(data);
      _event.add(null);
    }
  }

  void clear() {
    latest = null;
  }

  Future<void> waitForUpdate(Duration timeout) async {
    await _event.stream.first.timeout(timeout, onTimeout: () {});
  }

  void dispose() {
    _event.close();
  }
}

/// Same file-RX protocol as ble_file_tx.py (START/DATA/END + Notify flow control).
class BleTransferService {
  BleTransferService({
    this.fastMode = true,
    this.disableNotifyFc = false,
    this.chunkSizeOverride,
    this.packetDelayMs,
    int? flowControlWindowPkts,
    int? flowControlWaitStepPkts,
  })  : flowControlWindowPkts = flowControlWindowPkts,
        flowControlWaitStepPkts = flowControlWaitStepPkts;

  final bool fastMode;
  final bool disableNotifyFc;
  final int? chunkSizeOverride;
  final int? packetDelayMs;
  final int? flowControlWindowPkts;
  final int? flowControlWaitStepPkts;

  /// Reuse discoverServices result across consecutive transfers on one link.
  static String? _cachedRemoteId;
  static List<BluetoothService>? _cachedServices;
  static BluetoothCharacteristic? _cachedXferChar;

  static void clearGattCache() {
    _cachedRemoteId = null;
    _cachedServices = null;
    _cachedXferChar = null;
  }

  /// Share GATT table between upload and download on one link.
  static BluetoothCharacteristic? cachedXferChar(String remoteId) {
    if (_cachedRemoteId?.toUpperCase() == remoteId.toUpperCase() &&
        _cachedXferChar != null) {
      return _cachedXferChar;
    }
    return null;
  }

  static bool hasCachedGatt(String remoteId) =>
      cachedXferChar(remoteId) != null && _cachedServices != null;

  /// Cached discover result for [remoteId], if any.
  static ({List<BluetoothService> services, BluetoothCharacteristic char})?
      cachedGatt(String remoteId) {
    if (!hasCachedGatt(remoteId)) return null;
    return (services: _cachedServices!, char: _cachedXferChar!);
  }

  /// Connect + discover once after UI scan; keeps link open for stream/download.
  static Future<BluetoothDevice> warmupGatt(
    BluetoothDevice device,
    LogFn log,
  ) async {
    resetGattChain();
    await drainGattChain();
    final remoteId = device.remoteId.str;

    if (await BleConnect.isConnected(device) &&
        hasCachedGatt(remoteId)) {
      final cached = cachedXferChar(remoteId);
      if (cached != null &&
          (isWritableChar(cached) || isXferCharUuid(cached))) {
        log('GATT 已连接且已缓存，跳过 scan/discover');
        return device;
      }
      if (cached != null) {
        log('GATT 缓存 UUID 不匹配，重新 discover');
      }
      clearGattCache();
    }

    BluetoothDevice connected;
    if (await BleConnect.isConnected(device)) {
      log('已连接，跳过断连重扫');
      connected = device;
    } else {
      connected = await BleConnect.scanAndConnect(
        remoteId: remoteId,
        preferredDevice: device,
        log: log,
        disconnectAllFirst: true,
        requireFreshScan: false,
        scanRounds: 3,
        maxAttempts: 3,
      );
    }

    if (hasCachedGatt(remoteId) && await BleConnect.isConnected(connected)) {
      log('GATT 已缓存，保持连接');
      return connected;
    }

    await BleConnect.waitGattStable(connected, log);
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 1000 : 600),
    );

    final services = await discoverServicesWithRetry(
      connected,
      log,
      remoteId: remoteId,
      preferredDevice: connected,
    );
    final char = findXferChar(services);
    if (char == null) {
      throw StateError('未找到传输特征 ${BleProtocol.chrUuid}');
    }
    logCharProps(char, log);
    rememberGatt(remoteId, services, char);
    log('GATT 服务已预缓存（开始波形可跳过 discover）');
    return connected;
  }

  /// Light notify setup for STREAM — do not toggle CCCD off unless already on.
  static Future<void> prepareStreamNotify(
    BluetoothCharacteristic char,
    LogFn log,
  ) async {
    await drainGattChain();
    resetGattChain();
    if (char.isNotifying == true) {
      log('Notify 已开启，跳过 CCCD');
      return;
    }
    await enableNotifyWithRetry(char, log);
    await Future<void>.delayed(
      Duration(milliseconds: BleProtocol.notifySettleMs),
    );
    log('订阅 Notify 成功');
  }

  static void rememberGatt(
    String remoteId,
    List<BluetoothService> services,
    BluetoothCharacteristic char,
  ) {
    _cachedRemoteId = remoteId;
    _cachedServices = services;
    _cachedXferChar = char;
  }

  static Future<int> negotiateMtuForDevice(
    BluetoothDevice device,
    LogFn log, {
    bool trustPhoneMtu = true,
  }) async {
    var phoneMtu = device.mtuNow;
    log('phone mtuNow=$phoneMtu');
    if (trustPhoneMtu) {
      try {
        final req = await device
            .requestMtu(BleProtocol.defaultMtuRequest)
            .timeout(const Duration(seconds: 3));
        if (req > phoneMtu) phoneMtu = req;
      } catch (e) {
        log('requestMtu 3s 超时/失败: $e');
      }
      final now = device.mtuNow;
      if (now > phoneMtu) phoneMtu = now;
      if (phoneMtu > BleProtocol.boardAttMtuCap) {
        log('ATT MTU=$phoneMtu（下载/上传快速模式）');
        return phoneMtu;
      }
    }
    log(
      'ATT MTU=${BleProtocol.boardAttMtuCap} '
      '（板子 raw ATT；Android 勿信 mtuNow=$phoneMtu）',
    );
    return BleProtocol.boardAttMtuCap;
  }

  /// Android BLE stack rejects overlapping GATT ops (ERROR_GATT_WRITE_REQUEST_BUSY).
  static Future<void> _gattChain = Future<void>.value();

  static Future<T> _gattRun<T>(Future<T> Function() action) {
    final completer = Completer<T>();
    _gattChain = _gattChain.then((_) async {
      try {
        completer.complete(await action());
      } catch (e, st) {
        completer.completeError(e, st);
      }
    });
    return completer.future;
  }

  static bool isGattBusy(Object e) => _isGattBusy(e);

  static bool _isGattBusy(Object e) {
    // FlutterBluePlus includes the method name "discoverServices" in every
    // discover exception.  A disconnected link (fbp-code 6) is not a busy
    // link and must be rebuilt instead of retried on the same GATT instance.
    if (BleConnect.isDisconnectError(e)) return false;
    final s = e.toString();
    return s.contains('GATT_WRITE_REQUEST_BUSY') ||
        s.contains('ERROR_GATT_WRITE_REQUEST_BUSY') ||
        s.contains('gatt.writeDescriptor') ||
        s.contains('setNotifyValue') ||
        s.contains('discoverServices') ||
        _isGattTimeout(e);
  }

  static bool _isGattTimeout(Object e) {
    final s = e.toString();
    return s.contains('Timed out') &&
        (s.contains('setNotifyValue') ||
            s.contains('discoverServices') ||
            s.contains('write'));
  }

  static Future<void> drainGattChain() => _drainGattChain();

  static Future<void> _drainGattChain() async {
    try {
      await _gattChain.timeout(const Duration(seconds: 3));
    } catch (_) {
      _gattChain = Future<void>.value();
    }
  }

  static void resetGattChain() => _resetGattChain();

  static void _resetGattChain() {
    _gattChain = Future<void>.value();
  }

  static Future<T> gattRun<T>(Future<T> Function() action) => _gattRun(action);

  Future<void> sendFile({
    required BluetoothDevice device,
    required String filePath,
    required bool frameCrc,
    TransferController? controller,
    LogFn? onLog,
    ProgressFn? onProgress,
    /// Keep GATT connected after success so the next file skips reconnect.
    bool reuseConnection = false,
    bool disconnectAfter = true,
  }) async {
    final log = onLog ?? (_) {};
    final file = File(filePath);
    if (!await file.exists()) {
      throw StateError('File not found');
    }
    final payload = await file.readAsBytes();
    final basename = file.uri.pathSegments.last;
    final fileSize = payload.length;
    final fileCrc = BleProtocol.crc32Ieee(payload);

    log('目标 ${device.remoteId} …');
    log('mobile_app rev ${BleProtocol.appRev}');
    log('发送 $basename: $fileSize 字节, crc=0x${fileCrc.toRadixString(16).padLeft(8, '0')}');
    if (frameCrc) {
      log('Per-frame CRC16 enabled.');
    }

    Object? lastErr;
    var connectFail = false;
    var forceFresh = false;
    for (var attempt = 1; attempt <= BleProtocol.maxTransferAttempts; attempt++) {
      if (attempt > 1) {
        final wait = Duration(seconds: 2 * attempt);
        log('Retry $attempt/${BleProtocol.maxTransferAttempts} in ${wait.inSeconds}s...');
        await Future<void>.delayed(wait);
      }
      try {
        await _transferOnce(
          device: device,
          basename: basename,
          payload: payload,
          fileSize: fileSize,
          fileCrc: fileCrc,
          frameCrc: frameCrc,
          controller: controller,
          log: log,
          onProgress: onProgress,
          reuseConnection: reuseConnection && !forceFresh,
          disconnectAfter: disconnectAfter,
          forceFreshConnect: forceFresh,
        );
        return;
      } on TransferCancelledException {
        rethrow;
      } catch (e) {
        lastErr = e;
        final msg = e.toString();
        connectFail = msg.contains('GATT 连接') ||
            msg.contains('连接失败') ||
            msg.contains('Connection') ||
            msg.contains('requestMtu') ||
            msg.contains('未找到传输特征') ||
            _isGattBusy(e);
        log('Transfer failed (attempt $attempt/${BleProtocol.maxTransferAttempts}): $e');
        clearGattCache();
        _resetGattChain();
        if (_isGattBusy(e)) {
          forceFresh = true;
        }
        if (connectFail && attempt >= 2) {
          log('连接多次失败，请板子执行: start-my-server -U');
          break;
        }
      }
    }
    throw StateError('$lastErr');
  }

  Future<void> _transferOnce({
    required BluetoothDevice device,
    required String basename,
    required Uint8List payload,
    required int fileSize,
    required int fileCrc,
    required bool frameCrc,
    TransferController? controller,
    required LogFn log,
    ProgressFn? onProgress,
    bool reuseConnection = false,
    bool disconnectAfter = true,
    bool forceFreshConnect = false,
  }) async {
    _resetGattChain();
    await _drainGattChain();
    if (forceFreshConnect) {
      clearGattCache();
    }

    final connected = await BleConnect.connectForTransfer(
      remoteId: device.remoteId.str,
      preferredDevice: device,
      log: log,
      reuseIfConnected: reuseConnection,
      forceReconnect: forceFreshConnect,
    );
    final remoteId = connected.remoteId.str;
    final reusedLink = reuseConnection &&
        await BleConnect.isConnected(connected) &&
        _cachedRemoteId == remoteId &&
        _cachedXferChar != null;

    BluetoothCharacteristic? xferChar;
    StatusWatcher? watcher;
    StreamSubscription<List<int>>? notifySub;
    var transferOk = false;
    var notifyWasEnabled = false;

    try {
      await connected.connectionState
          .firstWhere((s) => s == BluetoothConnectionState.connected)
          .timeout(const Duration(seconds: 10));

      // Let Android GATT stack settle after connect / prior transfer.
      await Future<void>.delayed(
        Duration(milliseconds: reusedLink ? 350 : (Platform.isAndroid ? 900 : 600)),
      );

      List<BluetoothService>? services;
      if (reusedLink && _cachedServices != null && _cachedXferChar != null) {
        services = _cachedServices;
        xferChar = _cachedXferChar;
        log('复用已缓存 GATT 表，跳过 discoverServices');
      } else {
        services = await discoverServicesWithRetry(
          connected,
          log,
          remoteId: remoteId,
          preferredDevice: connected,
        );
        xferChar = _findTransferChar(services);
        if (xferChar == null) {
          throw StateError('未找到传输特征 ${BleProtocol.chrUuid}');
        }
        BleTransferService.rememberGatt(remoteId, services, xferChar);
        await BleConnect.requestHighPriorityIfAndroid(connected, log);
        log('GATT 服务已缓存（连续传文件可复用）');
      }
      final char = xferChar!;

      final mtu = await _gattRun(
        () => _negotiateMtu(
          connected,
          log,
          trustPhoneMtu: fastMode,
          reusedLink: reusedLink,
        ),
      );
      await Future<void>.delayed(
        Duration(milliseconds: Platform.isAndroid ? 150 : 80),
      );
      var mtuChunk = BleProtocol.maxChunkForMtu(mtu, frameCrc: frameCrc);

      final props = char.properties;
      log(
        '特征: write=${props.write} '
        'writeWithoutResponse=${props.writeWithoutResponse} '
        'notify=${props.notify}',
      );

      int effectiveChunk;
      int packetDelay;
      if (chunkSizeOverride != null) {
        effectiveChunk = chunkSizeOverride!;
        packetDelay = packetDelayMs ?? (fastMode ? 0 : BleProtocol.defaultTxDelayMs);
      } else if (fastMode) {
        effectiveChunk = math.min(mtuChunk, BleProtocol.fastChunkDefault).clamp(1, BleProtocol.safeChunkCap);
        packetDelay = packetDelayMs ?? 0;
      } else {
        effectiveChunk = mtuChunk.clamp(1, BleProtocol.safeChunkCap);
        packetDelay = packetDelayMs ?? BleProtocol.defaultTxDelayMs;
      }

      // Board raw ATT MTU is often 23 → max ~11 B/frame. Never exceed ATT limit.
      if (effectiveChunk > mtuChunk) {
        log(
          'chunk $effectiveChunk > ATT 上限 $mtuChunk (MTU=$mtu)，已自动缩小',
        );
        effectiveChunk = mtuChunk;
      }
      // Small MTU fallback: 2ms pacing (not 10ms); large MTU uses Notify FC.
      if (mtu <= BleProtocol.boardAttMtuCap && packetDelay < BleProtocol.defaultTxDelayMs) {
        packetDelay = BleProtocol.defaultTxDelayMs;
        log('小 MTU：${packetDelay}ms 包间隔');
      }

      final fcDefaults = BleProtocol.fcParams(fast: fastMode);
      var fcWindowReq = flowControlWindowPkts ?? fcDefaults.window;
      var fcWaitStep = flowControlWaitStepPkts ?? fcDefaults.step;
      var fcWindow = BleProtocol.clampFcWindow(fcWindowReq, effectiveChunk);

      var flowControl = false;
      // Notify FC only when ATT MTU is large enough; avoids Android CCCD busy on MTU 23.
      final tryNotifyFc = !disableNotifyFc &&
          props.notify &&
          mtu > BleProtocol.boardAttMtuCap;
      if (tryNotifyFc) {
        try {
          watcher = StatusWatcher();
          await enableNotifyWithRetry(char, log);
          notifySub = char.onValueReceived.listen(watcher.onData);
          await Future<void>.delayed(
            Duration(milliseconds: BleProtocol.notifySettleMs),
          );
          watcher.clear();
          notifyWasEnabled = true;
          flowControl = true;
          log('  subscribed board status (Notify, magic=0xA5 only)');
        } catch (e) {
          log('Status Notify subscribe failed; falling back to pacing ($e)');
          watcher?.dispose();
          watcher = null;
        }
      } else if (disableNotifyFc) {
        log('  Notify 流控已关闭；使用固定 pacing');
      } else if (mtu <= BleProtocol.boardAttMtuCap) {
        log('  MTU=$mtu：跳过 Notify，使用固定 pacing');
      }

      if (!flowControl && packetDelay <= 0) {
        packetDelay = fastMode
            ? BleProtocol.defaultTxDelayMs
            : BleProtocol.legacyFallbackTxDelayMs;
      }

      final totalPkts = (fileSize + effectiveChunk - 1) ~/ effectiveChunk;
      final estPackets = totalPkts + 2;
      final pace = flowControl
          ? 'Notify window=$fcWindow step=$fcWaitStep'
          : (packetDelay > 0 ? '${packetDelay}ms/pkt' : 'no delay (fast)');
      final crcNote = frameCrc ? 'CRC16/frame' : 'no frame CRC';
      log(
        'Connected. ATT MTU=$mtu, chunk=$effectiveChunk, '
        '~$estPackets packets, mode=write-without-response, '
        'pace=$pace, $crcNote',
      );
      if (flowControl && fcWindowReq != fcWindow) {
        log(
          '  FC window capped $fcWindowReq -> $fcWindow pkts '
          '(<= ${BleProtocol.boardRxBufBytes ~/ 1024}KB board buffer / chunk $effectiveChunk)',
        );
      }

      final sendName = BleProtocol.truncateFilenameForMtu(basename, mtu);
      if (sendName.isEmpty) {
        throw StateError(
          '文件名在 ATT MTU=$mtu 下无法放入 START 包 '
          '(最多 ${BleProtocol.maxStartFilenameBytes(mtu)} 字节)',
        );
      }
      if (sendName != basename) {
        log('文件名过长，START 截断为: $sendName');
      }
      log('发送 ABORT 复位板子 RX…');
      await _writeRetry(
        char,
        BleProtocol.buildAbort(),
        log,
        label: 'ABORT',
        mtu: mtu,
      );
      await Future<void>.delayed(const Duration(milliseconds: 250));

      final startPkt = BleProtocol.buildStart(sendName, fileSize, fileCrc);
      _checkAttPayload(mtu, startPkt, 'START');
      await _writeRetry(
        char,
        startPkt,
        log,
        label: 'START',
        mtu: mtu,
      );
      log('START 已发送 (${startPkt.length} 字节)');
      if (BleProtocol.startupExtraDelayMs > 0) {
        await Future<void>.delayed(
          Duration(milliseconds: BleProtocol.startupExtraDelayMs),
        );
      }

      if (flowControl && watcher != null) {
        try {
          final info = await _waitForFirstStatus(watcher);
          final stateName =
              BleProtocol.rxStateNames[info.state] ?? '${info.state}';
          log('  board status OK: state=$stateName, next_seq=${info.nextSeq}');
        } on TimeoutException catch (e) {
          log('  $e');
          log('  Falling back to fixed pacing (no Notify flow-control).');
          flowControl = false;
          await notifySub?.cancel();
          notifySub = null;
          try {
            await _gattRun(() => _setNotifyQuiet(char, false, log));
          } catch (_) {}
          watcher.dispose();
          watcher = null;
          if (packetDelay <= 0 && !fastMode) {
            packetDelay = BleProtocol.legacyFallbackTxDelayMs;
          } else if (packetDelay <= 0 && fastMode) {
            packetDelay = BleProtocol.defaultTxDelayMs;
          }
        }
      }

      final t0 = DateTime.now();
      var seq = 0;
      var nextReport = 0;
      final reportStep = math.max(
        effectiveChunk * BleProtocol.progressIntervalPkts,
        32768,
      );

      for (var off = 0; off < fileSize; off += effectiveChunk) {
        if (controller != null) await controller.waitIfPaused();

        final end = (off + effectiveChunk > fileSize) ? fileSize : off + effectiveChunk;
        final part = Uint8List.sublistView(payload, off, end);
        final dataPkt =
            BleProtocol.buildData(seq, part, frameCrc: frameCrc);
        _checkAttPayload(mtu, dataPkt, 'DATA $seq');
        await _writeRetry(
          char,
          dataPkt,
          log,
          label: 'DATA $seq',
          quiet: true,
          mtu: mtu,
        );
        seq++;
        final sent = end;

        if (flowControl && watcher != null) {
          if (BleProtocol.shouldFcWait(seq, fcWindow, fcWaitStep)) {
            await _waitForBoardNextSeq(
              watcher,
              seq - fcWindow,
              timeout: BleProtocol.flowControlTimeoutSec,
              label: 'flow window',
            );
          }
          if (packetDelay > 0) {
            await Future<void>.delayed(Duration(milliseconds: packetDelay));
          }
        } else if (seq <= BleProtocol.warmupPackets) {
          final d = packetDelay > BleProtocol.warmupDelayMs
              ? packetDelay
              : BleProtocol.warmupDelayMs;
          await Future<void>.delayed(Duration(milliseconds: d));
        } else if (packetDelay > 0) {
          await Future<void>.delayed(Duration(milliseconds: packetDelay));
        }

        if (sent >= nextReport || sent >= fileSize) {
          final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
          final rate = elapsed > 0 ? sent / elapsed : 0.0;
          log('  sent: $sent/$fileSize (${(rate / 1024).toStringAsFixed(1)} KB/s)');
          onProgress?.call(sent, fileSize, rate);
          nextReport = sent + reportStep;
        }
      }

      if (flowControl && watcher != null) {
        log('Waiting for board to catch up before END...');
        await _waitForBoardNextSeq(
          watcher,
          totalPkts,
          timeout: (fileSize / 8192.0).clamp(10.0, 120.0),
          label: 'final drain',
        );
      } else {
        final holdSec = (fileSize / 10000.0).clamp(0.2, 2.0);
        log('Legacy fallback hold ${holdSec.toStringAsFixed(1)}s before END...');
        await Future<void>.delayed(
          Duration(milliseconds: (holdSec * 1000).round()),
        );
      }

      await _writeRetry(
        char,
        BleProtocol.buildEnd(),
        log,
        label: 'END',
        mtu: mtu,
      );
      log('END sent');

      if (flowControl && watcher != null) {
        await _waitForBoardDoneNotify(watcher, fileSize, timeout: 10.0);
        log('Board confirmed transfer complete.');
      } else {
        log('Skipped GATT status read. Verify on board: ls -l ${BleProtocol.bleRxDir}/');
      }

      final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
      final rate = elapsed > 0 ? fileSize / elapsed : 0.0;
      onProgress?.call(fileSize, fileSize, rate);
      log(
        'Finished in ${elapsed.toStringAsFixed(1)}s '
        '(${(rate / 1024).toStringAsFixed(1)} KB/s). '
        'Check: ls -l ${BleProtocol.bleRxDir}/',
      );
      log('板子路径: ${BleProtocol.bleRxDir}/$sendName');

      log('发送 ABORT 复位板子 RX，准备下次传输…');
      await _writeRetry(
        char,
        BleProtocol.buildAbort(),
        log,
        label: 'ABORT',
        mtu: mtu,
      );
      await Future<void>.delayed(const Duration(milliseconds: 400));
      transferOk = true;
    } on TransferCancelledException {
      log('传输取消，发送 ABORT…');
      if (xferChar != null) {
        try {
          await _writeRetry(xferChar, BleProtocol.buildAbort(), log, label: 'ABORT');
        } catch (_) {}
      }
      rethrow;
    } finally {
      await notifySub?.cancel();
      final shouldDisconnect = disconnectAfter || !transferOk;
      // Keep CCCD unchanged when staying connected — avoids GATT_WRITE_REQUEST_BUSY.
      if (notifyWasEnabled && xferChar != null && shouldDisconnect) {
        await _gattRun(() => _setNotifyQuiet(xferChar!, false, log));
        await Future<void>.delayed(const Duration(milliseconds: 250));
      }
      watcher?.dispose();
      if (shouldDisconnect) {
        try {
          clearGattCache();
          if (Platform.isAndroid) {
            await Future<void>.delayed(const Duration(milliseconds: 400));
          }
          await connected.disconnect();
        } catch (_) {}
      } else {
        log('保持 GATT 连接（可继续传下一个文件）');
      }
    }
  }

  static Future<List<BluetoothService>> discoverServicesWithRetry(
    BluetoothDevice device,
    LogFn log, {
    bool clearCacheFirst = false,
    String? remoteId,
    BluetoothDevice? preferredDevice,
  }) async {
    var current = device;
    Object? lastErr;
    for (var i = 1; i <= 4; i++) {
      try {
        if (!await BleConnect.isConnected(current)) {
          if (remoteId != null) {
            log('discover 前连接已断，重新扫描连接…');
            current = await BleConnect.scanAndConnect(
              remoteId: remoteId,
              preferredDevice: preferredDevice ?? current,
              log: log,
              disconnectAllFirst: false,
            );
            await Future<void>.delayed(
              Duration(milliseconds: Platform.isAndroid ? 1000 : 600),
            );
          } else {
            throw StateError('device is not connected');
          }
        }

        if (clearCacheFirst && Platform.isAndroid) {
          try {
            await current.clearGattCache();
          } catch (_) {}
          await Future<void>.delayed(const Duration(milliseconds: 300));
        }

        await _drainGattChain();
        if (Platform.isAndroid) {
          await Future<void>.delayed(
            Duration(milliseconds: 800 + 500 * i),
          );
        }

        // Do not trust servicesList alone — Android often caches empty props.
        try {
          final existing = current.servicesList;
          if (existing.isNotEmpty) {
            final hit = findXferChar(existing);
            if (hit != null) {
              if (!Platform.isAndroid) {
                if (remoteId != null) rememberGatt(remoteId, existing, hit);
                return existing;
              }

              // The board uses a raw ATT server. After reconnect Android may
              // report all characteristic properties as false even though the
              // UUID-bound WWR handle is fully usable. Probe the binding with
              // an idempotent ABORT before forcing another service discovery.
              log(isWritableChar(hit)
                  ? 'servicesList 命中传输特征，验证重连句柄…'
                  : 'servicesList 属性为空，按 UUID 验证 raw ATT 句柄…');
              try {
                await writeProtocol(
                  hit,
                  BleProtocol.buildStreamAbort(),
                  log,
                  label: 'GATT PROBE',
                );
                if (remoteId != null) rememberGatt(remoteId, existing, hit);
                log('重连 GATT 句柄有效，跳过重复 discoverServices');
                return existing;
              } catch (e) {
                log('缓存句柄验证失败，执行 discoverServices: $e');
              }
            }
          }
        } catch (_) {}

        final services = await current.discoverServices().timeout(
          Duration(seconds: Platform.isAndroid ? 45 : 30),
        );
        final hit = findXferChar(services);
        if (hit != null) {
          logCharProps(hit, log);
          if (!isWritableChar(hit)) {
            log('discover 后特征仍不可写，打印完整 GATT 表:');
            logAllServices(services, log);
          }
        }
        return services;
      } catch (e) {
        lastErr = e;
        log('discoverServices 失败 $i/4: $e');
        final disconnected = BleConnect.isDisconnectError(e);
        if (i < 4 &&
            !disconnected &&
            (_isGattBusy(e) || _isGattTimeout(e))) {
          log('discover 同连接重试（不断开）…');
          await _drainGattChain();
          resetGattChain();
          await Future<void>.delayed(Duration(milliseconds: 1200 * i));
          continue;
        }
        if ((_isGattBusy(e) ||
                BleConnect.isDisconnectError(e) ||
                _isGattTimeout(e)) &&
            remoteId != null &&
            i < 4) {
          log('GATT 异常，断开并重新扫描连接…');
          clearGattCache();
          resetGattChain();
          try {
            if (await BleConnect.isConnected(current)) {
              await current.disconnect();
            }
          } catch (_) {}
          await Future<void>.delayed(Duration(milliseconds: 1200 * i));
          try {
            current = await BleConnect.scanAndConnect(
              remoteId: remoteId,
              preferredDevice: preferredDevice ?? current,
              log: log,
              disconnectAllFirst: true,
            );
            if (Platform.isAndroid) {
              try {
                await current.clearGattCache();
              } catch (_) {}
            }
            await Future<void>.delayed(
              Duration(milliseconds: Platform.isAndroid ? 1000 : 600),
            );
            continue;
          } catch (re) {
            lastErr = re;
            log('重连失败: $re');
          }
        }
        if (_isGattBusy(e) && i < 4) {
          await _drainGattChain();
          await Future<void>.delayed(Duration(milliseconds: 700 * i));
          continue;
        }
        if (BleConnect.isDisconnectError(e) && remoteId != null && i < 4) {
          try {
            current = await BleConnect.scanAndConnect(
              remoteId: remoteId,
              preferredDevice: preferredDevice ?? current,
              log: log,
              disconnectAllFirst: false,
            );
            await Future<void>.delayed(
              Duration(milliseconds: Platform.isAndroid ? 1000 : 600),
            );
            continue;
          } catch (re) {
            lastErr = re;
            log('重连失败: $re');
          }
        }
        if (i < 4) {
          await Future<void>.delayed(Duration(milliseconds: 800 * i));
        }
      }
    }
    throw StateError('discoverServices 失败: $lastErr');
  }

  static Future<void> _setNotifyQuiet(
    BluetoothCharacteristic char,
    bool enable,
    LogFn log,
  ) async {
    if (!char.properties.notify) return;
    try {
      await char.setNotifyValue(enable).timeout(const Duration(seconds: 8));
    } catch (e) {
      log('setNotifyValue($enable) 忽略: $e');
    }
  }

  static Future<void> resetNotifySession(
    BluetoothCharacteristic char,
    LogFn log,
  ) async {
    await _drainGattChain();
    resetGattChain();
    if (char.isNotifying == true) {
      log('复位 Notify：先关 CCCD…');
      await _gattRun(
        () => char.setNotifyValue(false).timeout(
          Duration(seconds: Platform.isAndroid ? 12 : 8),
        ),
      );
      await Future<void>.delayed(
        Duration(milliseconds: Platform.isAndroid ? 500 : 300),
      );
    }
    await enableNotifyWithRetry(char, log);
    await Future<void>.delayed(
      Duration(milliseconds: BleProtocol.notifySettleMs),
    );
    log('Notify 会话已复位');
  }

  static Future<void> resetBoardProtocol(
    BluetoothCharacteristic char,
    LogFn log,
  ) async {
    log('复位板子 STREAM/TX 状态…');
    try {
      await writeProtocol(char, BleProtocol.buildStreamAbort(), log,
          label: 'STREAM ABORT');
      await writeProtocol(char, BleProtocol.buildTxAbort(), log,
          label: 'TX ABORT');
    } catch (e) {
      log('板子复位忽略: $e');
    }
    await Future<void>.delayed(const Duration(milliseconds: 350));
  }

  static Future<void> enableNotifyWithRetry(
    BluetoothCharacteristic char,
    LogFn log, {
    bool skipIfNotifying = false,
  }) async {
    if (skipIfNotifying && char.isNotifying == true) {
      log('Notify 已开启，跳过 setNotifyValue');
      return;
    }
    Object? lastErr;
    for (var i = 1; i <= 8; i++) {
      try {
        if (i > 1) {
          await _drainGattChain();
          resetGattChain();
          await Future<void>.delayed(Duration(milliseconds: 700 * i));
          final msg = lastErr?.toString() ?? '';
          if (!msg.contains('primary service not found')) {
            await _gattRun(() => _setNotifyQuiet(char, false, log));
          }
          await Future<void>.delayed(Duration(milliseconds: 500 * i));
        }
        await _gattRun(
          () => char
              .setNotifyValue(true)
              .timeout(Duration(seconds: Platform.isAndroid ? 25 : 12)),
        );
        await Future<void>.delayed(
          Duration(milliseconds: Platform.isAndroid ? 350 : 200),
        );
        if (char.isNotifying != true) {
          throw StateError('setNotifyValue 返回成功但 isNotifying=false');
        }
        return;
      } catch (e) {
        lastErr = e;
        log('setNotifyValue(true) 失败 $i/8: $e');
        final msg = e.toString();
        if (msg.contains('primary service not found')) break;
        if (!_isGattBusy(e) && i >= 4) break;
      }
    }
    throw StateError('setNotifyValue: $lastErr');
  }

  Future<RxStatus> _waitForFirstStatus(
    StatusWatcher watcher, {
    double timeout = BleProtocol.firstStatusTimeoutSec,
  }) async {
    final deadline = DateTime.now().add(
      Duration(milliseconds: (timeout * 1000).round()),
    );

    while (DateTime.now().isBefore(deadline)) {
      final raw = watcher.latest;
      if (raw != null) {
        final info = BleProtocol.parseRxStatusFull(raw);
        if (info.isValid) return info;
      }

      final remaining = deadline.difference(DateTime.now());
      if (remaining <= Duration.zero) break;
      final waitMs = math.min(
        BleProtocol.flowControlPollMs,
        remaining.inMilliseconds,
      ).clamp(10, 500);
      try {
        await watcher.waitForUpdate(Duration(milliseconds: waitMs));
      } on TimeoutException {
        // keep polling
      }
    }

    throw TimeoutException(
      'No board status Notify (magic 0x${BleProtocol.statusMagic.toRadixString(16)}) '
      'within ${timeout.toStringAsFixed(0)}s after START. '
      'Check board: FILE RX START + my-server v3-notify8.',
    );
  }

  Future<RxStatus> _waitForBoardNextSeq(
    StatusWatcher watcher,
    int minNextSeq, {
    required double timeout,
    required String label,
  }) async {
    if (minNextSeq <= 0 && watcher.latest == null) {
      await _waitForFirstStatus(
        watcher,
        timeout: math.min(timeout, BleProtocol.firstStatusTimeoutSec),
      );
    }

    if (minNextSeq <= 0) {
      final raw = watcher.latest;
      if (raw != null) {
        return BleProtocol.parseRxStatusFull(raw);
      }
      throw TimeoutException('Timed out waiting for board $label (no status)');
    }

    final deadline = DateTime.now().add(Duration(milliseconds: (timeout * 1000).round()));
    RxStatus? lastInfo;

    while (DateTime.now().isBefore(deadline)) {
      final raw = watcher.latest;
      if (raw != null) {
        final info = BleProtocol.parseRxStatusFull(raw);
        lastInfo = info;
        if (info.isError) {
          final err = BleProtocol.rxErrNames[info.error] ??
              '0x${info.error.toRadixString(16)}';
          throw StateError(
            'Board RX error during $label: $err, next_seq=${info.nextSeq}, '
            'received=${info.received}/${info.expected}',
          );
        }
        if (info.nextSeq >= minNextSeq) {
          return info;
        }
      }

      final remaining = deadline.difference(DateTime.now());
      if (remaining <= Duration.zero) break;
      final waitMs = math.min(
        BleProtocol.flowControlPollMs,
        remaining.inMilliseconds,
      ).clamp(10, 500);
      try {
        await watcher.waitForUpdate(Duration(milliseconds: waitMs));
      } on TimeoutException {
        // keep polling
      }
    }

    var detail = 'no status';
    if (lastInfo != null) {
      final stateName = BleProtocol.rxStateNames[lastInfo.state] ??
          '${lastInfo.state}';
      detail =
          'last next_seq=${lastInfo.nextSeq}, '
          'received=${lastInfo.received}/${lastInfo.expected}, '
          'state=$stateName';
    }
    throw TimeoutException(
      'Timed out waiting for board $label >= seq $minNextSeq ($detail)',
    );
  }

  Future<RxStatus> _waitForBoardDoneNotify(
    StatusWatcher watcher,
    int fileSize, {
    required double timeout,
  }) async {
    final deadline = DateTime.now().add(Duration(milliseconds: (timeout * 1000).round()));
    RxStatus? lastInfo;

    while (DateTime.now().isBefore(deadline)) {
      final raw = watcher.latest;
      if (raw != null) {
        final info = BleProtocol.parseRxStatusFull(raw);
        lastInfo = info;
        if (info.isError) {
          final err = BleProtocol.rxErrNames[info.error] ??
              '0x${info.error.toRadixString(16)}';
          throw StateError('Board RX error after END: $err');
        }
        if (info.state == BleProtocol.rxStateDone && info.received == fileSize) {
          return info;
        }
      }

      final remaining = deadline.difference(DateTime.now());
      if (remaining <= Duration.zero) break;
      final waitMs = math.min(
        BleProtocol.flowControlPollMs,
        remaining.inMilliseconds,
      ).clamp(10, 500);
      try {
        await watcher.waitForUpdate(Duration(milliseconds: waitMs));
      } on TimeoutException {
        // keep polling
      }
    }

    var detail = 'no status';
    if (lastInfo != null) {
      final stateName = BleProtocol.rxStateNames[lastInfo.state] ??
          '${lastInfo.state}';
      detail =
          'state=$stateName, '
          'received=${lastInfo.received}/${lastInfo.expected}, '
          'next_seq=${lastInfo.nextSeq}';
    }
    throw TimeoutException('Timed out waiting for board DONE ($detail)');
  }

  Future<int> _negotiateMtu(
    BluetoothDevice device,
    LogFn log, {
    required bool trustPhoneMtu,
    required bool reusedLink,
  }) async {
    // requestConnectionPriority is already done in ble_connect on fresh links;
    // calling it again here often triggers GATT_WRITE_REQUEST_BUSY on Android.
    if (Platform.isAndroid && reusedLink) {
      try {
        await device.requestConnectionPriority(
          connectionPriorityRequest: ConnectionPriority.high,
        );
      } catch (_) {}
    }

    var phoneMtu = device.mtuNow;
    log('phone mtuNow=$phoneMtu');

    if (trustPhoneMtu) {
      try {
        final req = await device
            .requestMtu(BleProtocol.defaultMtuRequest)
            .timeout(const Duration(seconds: 3));
        if (req > phoneMtu) phoneMtu = req;
      } catch (e) {
        log('requestMtu 3s 超时/失败: $e');
      }
      final now = device.mtuNow;
      if (now > phoneMtu) phoneMtu = now;
      if (phoneMtu > BleProtocol.boardAttMtuCap) {
        log('ATT MTU=$phoneMtu（快速模式）');
        return phoneMtu;
      }
    }

    log(
      'ATT MTU=${BleProtocol.boardAttMtuCap} '
      '（板子 raw ATT；Android 勿信 mtuNow=$phoneMtu）',
    );
    return BleProtocol.boardAttMtuCap;
  }

  void _checkAttPayload(int mtu, List<int> data, String label) {
    final max = BleProtocol.maxAttPayload(mtu);
    if (data.length > max) {
      throw StateError(
        '$label: ${data.length} 字节 > ATT 上限 $max (MTU=$mtu)',
      );
    }
  }

  BluetoothCharacteristic? _findTransferChar(List<BluetoothService> services) =>
      findXferChar(services);

  static bool isXferCharUuid(BluetoothCharacteristic char) {
    final want = BleProtocol.chrUuid.replaceAll('-', '').toLowerCase();
    for (final got in _charUuidForms(char)) {
      if (got == want) return true;
    }
    return false;
  }

  static Iterable<String> _charUuidForms(BluetoothCharacteristic char) sync* {
    final u = char.uuid;
    yield u.str.replaceAll('-', '').toLowerCase();
    yield u.toString().replaceAll('-', '').toLowerCase();
  }

  static void logAllServices(List<BluetoothService> services, LogFn log) {
    for (final svc in services) {
      log('GATT 服务 ${svc.uuid.str}');
      for (final c in svc.characteristics) {
        final p = c.properties;
        log(
          '  特征 ${c.uuid.str}: write=${p.write} '
          'WWR=${p.writeWithoutResponse} notify=${p.notify}',
        );
      }
    }
  }

  static bool isWritableChar(BluetoothCharacteristic char) {
    final p = char.properties;
    return p.write || p.writeWithoutResponse;
  }

  static void logCharProps(BluetoothCharacteristic char, LogFn log) {
    final p = char.properties;
    log(
      '特征 ${char.uuid.str}: write=${p.write} '
      'writeWithoutResponse=${p.writeWithoutResponse} notify=${p.notify}',
    );
  }

  /// Shared phone->board write (STREAM / FILE RX / time sync).
  static Future<void> writeProtocol(
    BluetoothCharacteristic char,
    List<int> data,
    LogFn log, {
    String label = '',
    int? mtu,
  }) async {
    if (mtu != null) {
      final max = BleProtocol.maxAttPayload(mtu);
      if (data.length > max) {
        throw StateError(
          '$label: ${data.length} 字节 > ATT 上限 $max (MTU=$mtu)',
        );
      }
    }
    final props = char.properties;
    Object? lastErr;
    try {
      await _gattRun(() async {
        if (props.writeWithoutResponse) {
          await char.write(data, withoutResponse: true);
          return;
        }
        if (props.write) {
          await char.write(data, withoutResponse: false);
          return;
        }
        if (isXferCharUuid(char)) {
          // Raw L2CAP GATT: props often empty on Android; board supports WWR only.
          try {
            await char.write(data, withoutResponse: true);
            return;
          } on FlutterBluePlusException catch (e) {
            lastErr = e;
            throw StateError(
              '板子传输特征已找到但 Android 拒绝写入 ($e)。'
              '请重编刷入 my-server 后：关蓝牙→重扫→再试。',
            );
          }
        }
        try {
          await char.write(data, withoutResponse: true);
        } catch (e) {
          lastErr = e;
          await char.write(data, withoutResponse: false);
        }
      });
      if (label.isNotEmpty) {
        log('  $label op=0x${data[0].toRadixString(16)} (${data.length} B)');
      }
    } on FlutterBluePlusException catch (e) {
      lastErr = e;
      if (props.write && props.writeWithoutResponse) {
        await _gattRun(() => char.write(data, withoutResponse: false));
        if (label.isNotEmpty) {
          log('  $label: WWR 失败，已改用 write-with-response');
        }
        return;
      }
      log('写入失败 $label (${data.length} bytes): $lastErr');
      rethrow;
    }
  }

  static bool _uuidMatches(BluetoothCharacteristic char, String wantChr) {
    for (final got in _charUuidForms(char)) {
      if (got == wantChr) return true;
    }
    return false;
  }

  static BluetoothCharacteristic? findXferChar(List<BluetoothService> services) {
    final wantSvc = BleProtocol.svcUuid.replaceAll('-', '').toLowerCase();
    final wantChr = BleProtocol.chrUuid.replaceAll('-', '').toLowerCase();

    BluetoothCharacteristic? uuidMatch;

    for (final svc in services) {
      final su = svc.uuid.toString().replaceAll('-', '').toLowerCase();
      if (su != wantSvc) continue;
      for (final c in svc.characteristics) {
        if (!_uuidMatches(c, wantChr)) continue;
        if (isWritableChar(c)) return c;
        uuidMatch ??= c;
      }
    }

    for (final svc in services) {
      for (final c in svc.characteristics) {
        if (!_uuidMatches(c, wantChr)) continue;
        if (isWritableChar(c)) return c;
        uuidMatch ??= c;
      }
    }
    return uuidMatch;
  }

  Future<void> _writeRetry(
    BluetoothCharacteristic char,
    List<int> data,
    LogFn log, {
    required String label,
    bool quiet = false,
    int retries = 5,
    int? mtu,
  }) async {
    for (var attempt = 0; attempt < retries; attempt++) {
      try {
        await _write(char, data, log, quiet: quiet, label: label, mtu: mtu);
        return;
      } catch (e) {
        if (attempt + 1 >= retries) rethrow;
        final wait = Duration(milliseconds: (350 * (attempt + 1)).round());
        if (!quiet) {
          log('  $label retry ${attempt + 1}/$retries after ${wait.inMilliseconds}ms ($e)');
        }
        await Future<void>.delayed(wait);
      }
    }
  }

  Future<void> _write(
    BluetoothCharacteristic char,
    List<int> data,
    LogFn log, {
    bool quiet = false,
    String label = '',
    int? mtu,
  }) async {
    if (mtu != null) {
      _checkAttPayload(mtu, data, label);
    }
    final props = char.properties;
    Object? lastErr;
    try {
      if (props.writeWithoutResponse) {
        await _gattRun(() => char.write(data, withoutResponse: true));
        return;
      }
      if (props.write) {
        await _gattRun(() => char.write(data, withoutResponse: false));
        return;
      }
    } on FlutterBluePlusException catch (e) {
      lastErr = e;
      // Android: fall back to write-with-response if WWR fails.
      if (props.write && props.writeWithoutResponse) {
        try {
          await _gattRun(() => char.write(data, withoutResponse: false));
          if (!quiet) {
            log('  $label: WWR 失败，已改用 write-with-response');
          }
          return;
        } on FlutterBluePlusException catch (e2) {
          lastErr = e2;
        }
      }
      if (!quiet) {
        log('写入失败 $label (${data.length} bytes): $lastErr');
      }
      rethrow;
    }
    throw StateError('特征不可写');
  }
}
