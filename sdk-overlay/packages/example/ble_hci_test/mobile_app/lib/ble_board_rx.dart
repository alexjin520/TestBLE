import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_connect.dart';
import 'ble_time_sync.dart';
import 'ble_stream.dart';
import 'ble_transfer.dart';
import 'protocol.dart';

typedef LogFn = void Function(String msg);
typedef ProgressFn = void Function(int received, int total, double bytesPerSec);

class _BoardTxWatcher {
  final List<List<int>> _queue = [];
  final _signal = StreamController<void>.broadcast();
  final _lastSignal = StreamController<void>.broadcast();
  var _rxLogCount = 0;
  String? _lastRecordingName;

  void onData(List<int> data) {
    if (data.isEmpty) return;
    final op = data[0];
    if (_rxLogCount < 8) {
      _rxLogCount++;
    }
    if (op == BleProtocol.opTxLastRsp) {
      final name = BleProtocol.parseTxLastRsp(data);
      if (name != null && name.isNotEmpty) {
        _lastRecordingName = name;
        if (!_lastSignal.isClosed) {
          _lastSignal.add(null);
        }
      }
      return;
    }
    if (op == BleProtocol.opTxStart ||
        op == BleProtocol.opTxData ||
        op == BleProtocol.opTxEnd) {
      _queue.add(List<int>.from(data));
      if (!_signal.isClosed) {
        _signal.add(null);
      }
    }
  }

  int get rxLogCount => _rxLogCount;

  Future<List<int>> nextPacket(Duration timeout) async {
    final deadline = DateTime.now().add(timeout);
    while (_queue.isEmpty) {
      final remaining = deadline.difference(DateTime.now());
      if (remaining <= Duration.zero) {
        throw TimeoutException('等待板子 FILE TX 包超时');
      }
      try {
        await _signal.stream.first.timeout(remaining);
      } on TimeoutException {
        if (_queue.isNotEmpty) break;
        rethrow;
      }
    }
    return _queue.removeAt(0);
  }

  bool get hasPackets => _queue.isNotEmpty;

  List<int>? tryPop() => _queue.isNotEmpty ? _queue.removeAt(0) : null;

  Future<String> waitForLastName(Duration timeout) async {
    final deadline = DateTime.now().add(timeout);
    while (_lastRecordingName == null || _lastRecordingName!.isEmpty) {
      final remaining = deadline.difference(DateTime.now());
      if (remaining <= Duration.zero) {
        throw TimeoutException('等待 FILE_TX_LAST_RSP 超时');
      }
      try {
        await _lastSignal.stream.first.timeout(remaining);
      } on TimeoutException {
        if (_lastRecordingName != null && _lastRecordingName!.isNotEmpty) {
          break;
        }
        rethrow;
      }
    }
    return _lastRecordingName!;
  }

  void clear() {
    _queue.clear();
    _lastRecordingName = null;
  }

  void dispose() {
    _signal.close();
    _lastSignal.close();
  }
}

/// Board -> phone file download (FILE TX notify stream).
class BleBoardRxService {
  BleBoardRxService({this.frameCrc = true});

  final bool frameCrc;

  static const _testBleDir = 'TestBLE';

  static Future<void> _assertFileSize(File file, int expected) async {
    if (!await file.exists()) {
      throw StateError('文件未创建: ${file.path}');
    }
    final len = await file.length();
    if (len != expected) {
      throw StateError('文件大小不符: $len != $expected (${file.path})');
    }
  }

  /// Save to Download/TestBLE on Android (no extra packages).
  static Future<({
    String localPath,
    String displayPath,
    bool publicVisible,
  })> saveDownloadedFile({
    required String filename,
    required Uint8List payload,
    LogFn? onLog,
  }) async {
    final log = onLog ?? (_) {};
    final safeName = filename.split(RegExp(r'[/\\]')).last;
    if (safeName.isEmpty || safeName == '.' || safeName == '..') {
      throw StateError('无效文件名: $filename');
    }

    if (Platform.isAndroid) {
      const roots = [
        '/storage/emulated/0/Download',
        '/sdcard/Download',
      ];
      for (final root in roots) {
        try {
          final dir = Directory('$root/$_testBleDir');
          await dir.create(recursive: true);
          final file = File('${dir.path}/$safeName');
          await file.writeAsBytes(payload, flush: true);
          await _assertFileSize(file, payload.length);
          log('已保存: ${file.path} (${payload.length} B)');
          return (
            localPath: file.path,
            displayPath: '下载/TestBLE/$safeName',
            publicVisible: true,
          );
        } catch (e) {
          log('无法写入 $root/$_testBleDir: $e');
        }
      }
      throw StateError(
        '无法写入「下载/TestBLE/$safeName」。'
        '请在系统设置中允许存储权限，或用文件管理器确认 Download/TestBLE 可写。',
      );
    }

    final dir = Directory('${Directory.systemTemp.path}/$_testBleDir');
    await dir.create(recursive: true);
    final file = File('${dir.path}/$safeName');
    await file.writeAsBytes(payload, flush: true);
    await _assertFileSize(file, payload.length);
    return (
      localPath: file.path,
      displayPath: file.path,
      publicVisible: false,
    );
  }

  Future<ReceiveResult> receiveFile({
    required BluetoothDevice device,
    String? remoteFilename,
    bool queryLastFirst = false,
    TransferController? controller,
    LogFn? onLog,
    ProgressFn? onProgress,
    bool reuseConnection = true,
    bool disconnectAfter = false,
  }) async {
    final log = onLog ?? (_) {};
    if (!queryLastFirst && (remoteFilename == null || remoteFilename.isEmpty)) {
      throw ArgumentError('remoteFilename required unless queryLastFirst');
    }
    log(
      queryLastFirst
          ? '下载最近录制 (${BleProtocol.appRev})'
          : '下载请求: $remoteFilename (${BleProtocol.appRev})',
    );

    Object? lastErr;
    var forceFresh = false;
    for (var attempt = 1; attempt <= BleProtocol.maxTransferAttempts; attempt++) {
      try {
        if (attempt > 1) {
          log('下载重试 $attempt/${BleProtocol.maxTransferAttempts}…');
        }
        return await _receiveOnce(
          device: device,
          remoteFilename: remoteFilename,
          queryLastFirst: queryLastFirst,
          controller: controller,
          log: log,
          onProgress: onProgress,
          reuseConnection: reuseConnection && !forceFresh,
          disconnectAfter: disconnectAfter,
          forceFreshConnect: forceFresh,
        );
      } on TransferCancelledException {
        rethrow;
      } catch (e) {
        lastErr = e;
        log('下载失败 (attempt $attempt): $e');
        BleTransferService.clearGattCache();
        BleTransferService.resetGattChain();
        final msg = e.toString();
        if (BleTransferService.isGattBusy(e) ||
            msg.contains('setNotifyValue') ||
            msg.contains('primary service not found') ||
            msg.contains('discoverServices') ||
            msg.contains('Connection') ||
            msg.contains('disconnected')) {
          forceFresh = true;
        }
        if (attempt < BleProtocol.maxTransferAttempts) {
          await Future<void>.delayed(
            Duration(milliseconds: Platform.isAndroid ? 1500 : 800),
          );
        }
      }
    }
    throw StateError('$lastErr');
  }

  Future<ReceiveResult> _receiveOnce({
    required BluetoothDevice device,
    String? remoteFilename,
    bool queryLastFirst = false,
    TransferController? controller,
    required LogFn log,
    ProgressFn? onProgress,
    bool reuseConnection = true,
    bool disconnectAfter = false,
    bool forceFreshConnect = false,
  }) async {
    BleTransferService.resetGattChain();
    await BleTransferService.drainGattChain();
    if (forceFreshConnect) {
      BleTransferService.clearGattCache();
    }

    final connected = await BleConnect.connectForTransfer(
      remoteId: device.remoteId.str,
      preferredDevice: device,
      log: log,
      reuseIfConnected: reuseConnection,
      forceReconnect: forceFreshConnect,
    );

    _BoardTxWatcher? watcher;
    StreamSubscription<List<int>>? notifySub;
    BluetoothCharacteristic? xferChar;
    var ok = false;

    try {
      await connected.connectionState
          .firstWhere((s) => s == BluetoothConnectionState.connected)
          .timeout(const Duration(seconds: 10));

      final remoteId = connected.remoteId.str;
      final reusedLink = reuseConnection &&
          !forceFreshConnect &&
          await BleConnect.isConnected(connected) &&
          BleTransferService.cachedXferChar(remoteId) != null;

      await Future<void>.delayed(
        Duration(
          milliseconds: reusedLink
              ? 500
              : (Platform.isAndroid ? 900 : 600),
        ),
      );

      xferChar = BleTransferService.cachedXferChar(remoteId);
      if (xferChar != null && reusedLink) {
        log('复用上传 GATT 缓存，跳过 discoverServices');
      } else {
        final services = await BleTransferService.discoverServicesWithRetry(
          connected,
          log,
          clearCacheFirst: forceFreshConnect,
          remoteId: remoteId,
          preferredDevice: connected,
        );
        xferChar = _findTransferChar(services);
        if (xferChar == null) {
          throw StateError('未找到传输特征 ${BleProtocol.chrUuid}');
        }
        BleTransferService.rememberGatt(remoteId, services, xferChar);
        await BleConnect.requestHighPriorityIfAndroid(connected, log);
        log('GATT 服务已缓存');
      }
      final char = xferChar!;

      await BleTransferService.gattRun(
        () => BleTransferService.negotiateMtuForDevice(
          connected,
          log,
          trustPhoneMtu: true,
        ),
      );
      await Future<void>.delayed(
        Duration(milliseconds: Platform.isAndroid ? 300 : 150),
      );

      if (!char.properties.notify) {
        throw StateError('板子特征不支持 Notify，无法下载');
      }

      await BleTimeSync.pushToCharacteristic(char, log);

      watcher = _BoardTxWatcher();

      await notifySub?.cancel();
      notifySub = null;
      if (char.isNotifying == true && reusedLink) {
        log('复用已开启 Notify，跳过 CCCD');
      } else if (char.isNotifying == true) {
        log('Notify 已开，先关再重开（避免 CCCD 卡死）');
        try {
          await BleTransferService.gattRun(
            () => char.setNotifyValue(false).timeout(const Duration(seconds: 8)),
          );
          await Future<void>.delayed(const Duration(milliseconds: 400));
        } catch (e) {
          log('关 Notify 忽略: $e');
        }
        await BleTransferService.enableNotifyWithRetry(char, log);
      } else {
        await BleTransferService.enableNotifyWithRetry(char, log);
      }
      log('订阅 Notify 成功 (CCCD=0x01)');
      await Future<void>.delayed(
        Duration(milliseconds: BleProtocol.notifySettleMs),
      );
      notifySub = char.onValueReceived.listen(watcher.onData);
      watcher.clear();

      log('发送 STREAM/TX ABORT 复位板子…');
      try {
        await _write(
          char,
          BleProtocol.buildStreamAbort(),
          log,
          label: 'STREAM ABORT',
        );
      } catch (e) {
        log('STREAM ABORT 忽略: $e');
      }
      await _write(char, BleProtocol.buildTxAbort(), log, label: 'TX ABORT');
      await Future<void>.delayed(const Duration(milliseconds: 300));
      watcher.clear();

      late final String filename;
      if (queryLastFirst) {
        log('查询最近录制 (FILE_TX_LAST)…');
        for (var attempt = 1; attempt <= 3; attempt++) {
          if (attempt > 1) {
            log('查询最近录制重试 $attempt/3…');
            await Future<void>.delayed(Duration(seconds: attempt));
            watcher.clear();
          }
          await _write(
            char,
            BleProtocol.buildTxLastReq(),
            log,
            label: 'TX LAST REQ',
          );
          try {
            filename = await watcher.waitForLastName(
              const Duration(seconds: 12),
            );
            log('最近录制: $filename');
            break;
          } on TimeoutException {
            watcher.clear();
            if (attempt >= 3) {
              throw StateError(
                '查询最近录制无响应。请先停止采集再下载；'
                '板子 cat /app_data/ble_tx/.last 应有文件名',
              );
            }
          }
        }
        watcher.clear();
      } else {
        filename = remoteFilename!;
      }

      log('发送 FILE_TX_REQ…');
      await _write(
        char,
        BleProtocol.buildTxReq(filename),
        log,
        label: 'TX REQ',
      );

      TxStartInfo? start;
      final preStartData = <List<int>>[];
      final startDeadline = DateTime.now().add(
        Duration(milliseconds: (BleProtocol.txStartTimeoutSec * 1000).round()),
      );
      while (start == null && DateTime.now().isBefore(startDeadline)) {
        final remaining = startDeadline.difference(DateTime.now());
        if (remaining <= Duration.zero) break;
        final raw = await watcher.nextPacket(remaining);
        if (raw[0] == BleProtocol.opTxStart) {
          start = BleProtocol.parseTxStart(raw);
        } else if (raw[0] == BleProtocol.opTxData) {
          preStartData.add(raw);
        }
      }
      if (start == null) {
        throw StateError('无效的 FILE_TX_START / 超时');
      }
      if (preStartData.isNotEmpty) {
        log('缓冲 START 前到达的 ${preStartData.length} 个 DATA 包');
      }
      log(
        'FILE_TX_START: ${start.filename} ${start.size} 字节 '
        'crc=0x${start.crc32.toRadixString(16).padLeft(8, '0')}',
      );

      var lastAckSent = -1;
      var ackBusy = false;
      int? pendingAckSeq;
      final buffer = BytesBuilder(copy: false);
      final pendingBySeq = <int, Uint8List>{};
      var nextSeq = 0;
      final t0 = DateTime.now();
      var nextReport = 0;
      final reportStep = 4 * 1024;
      var sawEarlyEnd = false;
      final estPkts = (start.size / BleProtocol.fastChunkDefault).ceil() + 1;

      void reportProgress(int received) {
        if (received >= nextReport || received >= start!.size) {
          final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
          final rate = elapsed > 0 ? received / elapsed : 0.0;
          log(
            '  recv: $received/${start!.size} '
            '(${(rate / 1024).toStringAsFixed(1)} KB/s)',
          );
          onProgress?.call(received, start!.size, rate);
          nextReport = received + reportStep;
        }
      }

      late final Future<void> Function(int ackSeq, {bool force}) flushAck;

      void absorbDataFrame(TxDataFrame frame) {
        if (frame.seq < nextSeq) {
          return;
        }
        pendingBySeq[frame.seq] = frame.chunk;
        while (pendingBySeq.containsKey(nextSeq)) {
          buffer.add(pendingBySeq.remove(nextSeq)!);
          nextSeq++;
          reportProgress(buffer.length);
          if (nextSeq > 0 && nextSeq % BleProtocol.txFcWindowPkts == 0) {
            unawaited(flushAck(nextSeq));
          }
        }
      }

      void handleRaw(List<int> raw) {
        if (raw[0] == BleProtocol.opTxEnd) {
          if (buffer.length >= start!.size) {
            return;
          }
          sawEarlyEnd = true;
          log(
            '收到 END 过早: ${buffer.length}/${start!.size}，'
            '队列=${watcher!.hasPackets ? "有" : "空"} pending=${pendingBySeq.length}',
          );
          return;
        }
        if (raw[0] != BleProtocol.opTxData) {
          return;
        }

        final frame = BleProtocol.parseTxData(raw, expectFrameCrc: frameCrc);
        if (frame == null) {
          log('跳过无效 DATA (${raw.length} B)');
          return;
        }
        if (frame.seq >= estPkts && nextSeq == 0 && buffer.length == 0) {
          log('丢弃残留 DATA seq=${frame.seq}');
          return;
        }
        absorbDataFrame(frame);
      }

      flushAck = (int ackSeq, {bool force = false}) async {
        if (!force &&
            ackSeq > 0 &&
            ackSeq % BleProtocol.txFcWindowPkts != 0) {
          return;
        }
        if (ackSeq <= lastAckSent) return;

        pendingAckSeq = pendingAckSeq == null
            ? ackSeq
            : math.max(pendingAckSeq!, ackSeq);
        if (ackBusy) return;
        ackBusy = true;

        try {
          while (pendingAckSeq != null && pendingAckSeq! > lastAckSent) {
            final target = pendingAckSeq!;
            pendingAckSeq = null;

            for (var i = 0; i < 4; i++) {
              while (watcher!.hasPackets) {
                handleRaw(watcher.tryPop()!);
              }
              if (!watcher.hasPackets) break;
              await Future<void>.delayed(const Duration(milliseconds: 5));
            }
            await Future<void>.delayed(
              Duration(milliseconds: Platform.isAndroid ? 10 : 8),
            );
            await _writeRetry(
              char,
              BleProtocol.buildTxAck(target),
              log,
              label: 'TX ACK($target)',
            );
            lastAckSent = target;
            await Future<void>.delayed(
              Duration(milliseconds: Platform.isAndroid ? 8 : 6),
            );
          }
        } finally {
          ackBusy = false;
          if (pendingAckSeq != null && pendingAckSeq! > lastAckSent) {
            unawaited(flushAck(pendingAckSeq!, force: true));
          }
        }
      };

      log(
        'ACK 开窗流控 window=${BleProtocol.txFcWindowPkts} '
        '(板子停窗后写 ACK)',
      );
      await Future<void>.delayed(const Duration(milliseconds: 80));
      await flushAck(0, force: true);

      for (final raw in preStartData) {
        handleRaw(raw);
      }

      while (buffer.length < start.size) {
        if (controller != null) await controller.waitIfPaused();

        while (watcher!.hasPackets) {
          handleRaw(watcher.tryPop()!);
          if (buffer.length >= start.size) break;
        }
        if (buffer.length >= start.size) break;

        if (nextSeq > lastAckSent &&
            nextSeq > 0 &&
            nextSeq % BleProtocol.txFcWindowPkts == 0) {
          unawaited(flushAck(nextSeq));
        }

        if (!watcher!.hasPackets && pendingBySeq.isNotEmpty) {
          final waitSeq = pendingBySeq.keys.reduce(math.min);
          if (waitSeq > nextSeq) {
            log('等待 seq=$nextSeq (已收到乱序 seq=$waitSeq…)');
          }
        }

        final raw = await watcher.nextPacket(
          Duration(
            milliseconds: (BleProtocol.txPacketTimeoutSec * 1000).round(),
          ),
        );
        handleRaw(raw);

        if (sawEarlyEnd &&
            !watcher.hasPackets &&
            buffer.length < start.size &&
            pendingBySeq.isEmpty) {
          log(
            '传输中断: recv=${buffer.length}/${start.size} '
            'notify_rx=${watcher.rxLogCount}',
          );
          throw StateError(
            '传输中断: recv=${buffer.length}/${start.size} '
            '(END 后无更多 DATA)',
          );
        }
      }

      final payload = buffer.toBytes();
      if (payload.length != start.size) {
        throw StateError(
          '大小不符: recv=${payload.length} expected=${start.size}',
        );
      }
      final crc = BleProtocol.crc32Ieee(payload);
      if (crc != start.crc32) {
        throw StateError(
          'CRC32 不符: got=0x${crc.toRadixString(16)} '
          'expected=0x${start.crc32.toRadixString(16)}',
        );
      }

      final outName =
          start.filename.isNotEmpty ? start.filename : filename;
      final saved = await saveDownloadedFile(
        filename: outName,
        payload: payload,
        onLog: log,
      );

      final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
      final rate = elapsed > 0 ? payload.length / elapsed : 0.0;
      onProgress?.call(payload.length, start.size, rate);
      log(
        '下载完成: ${saved.displayPath} '
        '(${(rate / 1024).toStringAsFixed(1)} KB/s)',
      );
      if (!saved.publicVisible) {
        log('请到 App 日志查看「内部路径」，或用文件管理器搜索 $outName');
      }
      ok = true;
      return ReceiveResult(
        localPath: saved.localPath,
        displayPath: saved.displayPath,
        publicVisible: saved.publicVisible,
        filename: outName,
        size: payload.length,
        crc32: crc,
        bytesPerSec: rate,
      );
    } on TransferCancelledException {
      if (xferChar != null) {
        try {
          await _write(
            xferChar,
            BleProtocol.buildTxAbort(),
            log,
            label: 'TX ABORT',
          );
        } catch (_) {}
      }
      rethrow;
    } finally {
      await notifySub?.cancel();
      if (!ok) {
        BleTransferService.clearGattCache();
      }
      // disconnectAfter=false: keep link + Notify for retry (main.dart download path).
      if (disconnectAfter) {
        if (xferChar != null && !ok) {
          try {
            await BleTransferService.gattRun(() => xferChar!.setNotifyValue(false));
          } catch (_) {}
        }
        try {
          await connected.disconnect();
        } catch (_) {}
      } else if (ok) {
        log('保持 GATT 连接');
        if (xferChar != null) {
          await BleTransferService.resetBoardProtocol(xferChar, log);
          await BleTransferService.prepareStreamNotify(xferChar, log);
        }
      } else {
        log('下载失败，保持连接供重试');
      }
      watcher?.dispose();
    }
  }

  /// Download latest fNIRS Hangzhou.bin (query path via FNIRS, then FILE TX).
  Future<ReceiveResult> downloadHangzhouRecording({
    required BluetoothDevice device,
    required Future<String> Function() queryPath,
    TransferController? controller,
    LogFn? onLog,
    ProgressFn? onProgress,
    BleStreamService? streamService,
  }) async {
    final log = onLog ?? (_) {};
    await streamService?.detachNotifyForTransfer(log);
    await streamService?.stop(
      onLog: log,
      keepLink: true,
      prefetchLast: false,
    );

    final path = await queryPath();
    final name = path.replaceAll('\\', '/').split('/').last;
    if (name.isEmpty) {
      throw StateError('板子返回无效录制路径');
    }
    log('Hangzhou 录制: $path');

    return receiveFile(
      device: streamService?.linkedDevice ?? device,
      remoteFilename: name,
      controller: controller,
      onLog: onLog,
      onProgress: onProgress,
      reuseConnection: true,
      disconnectAfter: false,
    );
  }

  /// Query last recording on board, then FILE TX download on the same notify session.
  Future<ReceiveResult> downloadLastRecording({
    required BluetoothDevice device,
    TransferController? controller,
    LogFn? onLog,
    ProgressFn? onProgress,
    BleStreamService? streamService,
  }) async {
    final log = onLog ?? (_) {};
    await streamService?.detachNotifyForTransfer(log);
    streamService?.lastRecordingBasename = null;

    final linked = streamService?.linkedDevice ?? device;

    return receiveFile(
      device: linked,
      queryLastFirst: true,
      controller: controller,
      onLog: onLog,
      onProgress: onProgress,
      reuseConnection: true,
      disconnectAfter: false,
    );
  }

  BluetoothCharacteristic? _findTransferChar(List<BluetoothService> services) {
    final target = BleProtocol.chrUuid.toLowerCase();
    for (final svc in services) {
      for (final c in svc.characteristics) {
        if (c.uuid.str.toLowerCase() == target) return c;
      }
    }
    return null;
  }

  Future<void> _writeRetry(
    BluetoothCharacteristic char,
    Uint8List data,
    LogFn log, {
    required String label,
    int retries = 6,
  }) async {
    for (var attempt = 0; attempt < retries; attempt++) {
      try {
        await _write(char, data, log, label: label);
        return;
      } catch (e) {
        if (attempt + 1 >= retries) rethrow;
        final wait = Duration(
          milliseconds: BleTransferService.isGattBusy(e)
              ? (400 * (attempt + 1)).round()
              : (150 * (attempt + 1)).round(),
        );
        log('  $label retry ${attempt + 1}/$retries after ${wait.inMilliseconds}ms ($e)');
        await Future<void>.delayed(wait);
      }
    }
  }

  Future<void> _write(
    BluetoothCharacteristic char,
    Uint8List data,
    LogFn log, {
    required String label,
  }) async {
    await BleTransferService.gattRun(() async {
      if (char.properties.writeWithoutResponse) {
        await char.write(data, withoutResponse: true);
      } else {
        await char.write(data, withoutResponse: false);
      }
    });
    log('  $label sent (${data.length} B)');
  }
}

class ReceiveResult {
  const ReceiveResult({
    required this.localPath,
    required this.displayPath,
    required this.publicVisible,
    required this.filename,
    required this.size,
    required this.crc32,
    required this.bytesPerSec,
  });

  final String localPath;
  final String displayPath;
  final bool publicVisible;
  final String filename;
  final int size;
  final int crc32;
  final double bytesPerSec;
}
