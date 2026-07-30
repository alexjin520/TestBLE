import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_transfer.dart';
import 'protocol.dart';

typedef LiveSyncLogFn = void Function(String message);
typedef LiveSyncProgressFn = void Function(
  int received,
  int? finalSize,
  double bytesPerSec,
);

class LiveSyncResult {
  const LiveSyncResult({
    required this.filename,
    required this.localPath,
    required this.displayPath,
    required this.size,
    required this.crc32,
    required this.bytesPerSec,
  });

  final String filename;
  final String localPath;
  final String displayPath;
  final int size;
  final int crc32;
  final double bytesPerSec;
}

/// Reliable copy of the board's growing Hangzhou.bin.
///
/// STREAM notifications still drive the waveform. LIVE DATA notifications are
/// written directly to a .part file, ACKed in windows, and renamed only after
/// SAMPLE_OFF has sealed the board file and the final size/CRC match.
class BleLiveSyncService {
  BluetoothCharacteristic? _char;
  StreamSubscription<BluetoothConnectionState>? _connectionSub;
  TransferController? _controller;
  RandomAccessFile? _output;
  String? _filename;
  String? _partPath;
  String? _finalPath;
  String? _displayPath;
  LiveSyncLogFn _log = (_) {};
  LiveSyncProgressFn? _onProgress;
  Completer<void>? _startWaiter;
  Completer<LiveSyncResult>? _doneWaiter;
  Future<void> _serial = Future<void>.value();
  DateTime? _startedAt;
  DateTime? _lastProgressAt;
  int _nextProgressBytes = 0;
  int _nextSeq = 0;
  int _received = 0;
  int _crc32 = 0;
  int? _finalSize;
  int? _finalCrc32;
  bool _active = false;
  bool _started = false;
  bool _finishing = false;
  bool _failing = false;
  bool _expectingFinalization = false;
  Timer? _tailWatchdog;
  Duration _tailTimeout = const Duration(seconds: 12);

  bool get isActive => _active;
  int get receivedBytes => _received;
  int? get finalSize => _finalSize;
  String? get filename => _filename;

  Future<LiveSyncResult> get done {
    final waiter = _doneWaiter;
    if (waiter == null) {
      return Future<LiveSyncResult>.error(
        StateError('LIVE 文件同步尚未启动'),
      );
    }
    return waiter.future;
  }

  Future<void> start({
    required BluetoothDevice device,
    required BluetoothCharacteristic characteristic,
    required TransferController controller,
    LiveSyncLogFn? onLog,
    LiveSyncProgressFn? onProgress,
  }) async {
    if (_active) {
      throw StateError('LIVE 文件同步已经启动');
    }

    _char = characteristic;
    _controller = controller;
    _log = onLog ?? (_) {};
    _onProgress = onProgress;
    _startWaiter = Completer<void>();
    _doneWaiter = Completer<LiveSyncResult>();
    _serial = Future<void>.value();
    _startedAt = DateTime.now();
    _lastProgressAt = null;
    _nextProgressBytes = 0;
    _nextSeq = 0;
    _received = 0;
    _crc32 = 0;
    _finalSize = null;
    _finalCrc32 = null;
    _active = true;
    _started = false;
    _finishing = false;
    _failing = false;
    _expectingFinalization = false;
    _tailWatchdog?.cancel();
    await _connectionSub?.cancel();
    _connectionSub = device.connectionState.listen((state) {
      if (_active && state == BluetoothConnectionState.disconnected) {
        unawaited(_fail(StateError('BLE 连接已断开，LIVE 尾包未完成')));
      }
    });

    Object? lastError;
    for (var attempt = 1; attempt <= 5; attempt++) {
      try {
        if (attempt > 1) {
          _log('LIVE 同步启动重试 $attempt/5…');
          await _write(BleProtocol.buildLiveAbort(), 'LIVE ABORT');
          await Future<void>.delayed(const Duration(milliseconds: 400));
          await _serial;
          await _resetStartAttempt();
        }
        await _write(BleProtocol.buildLiveReq(), 'LIVE REQ');
        await _startWaiter!.future.timeout(const Duration(seconds: 3));
        _log('完整文件同步已启动：采集与下载并行');
        return;
      } catch (e) {
        lastError = e;
        if (_started) return;
        if (!_active) break;
      }
    }

    final error = StateError('LIVE 同步启动失败: $lastError');
    _active = false;
    await _abortBoardQuiet();
    await _closeWatchers();
    await _closeOutput();
    _log('$error');
    throw error;
  }

  /// Called by BleStreamService for opcodes 0x2A–0x2E.
  void onNotify(List<int> data) {
    if (!_active || data.isEmpty) return;
    final op = data[0];
    if (op < BleProtocol.opLiveStart || op > BleProtocol.opLiveAbort) {
      return;
    }

    final packet = List<int>.from(data);
    _serial = _serial.then((_) => _handlePacket(packet)).catchError(
      (Object error, StackTrace stackTrace) async {
        await _fail(error);
      },
    );
  }

  /// SAMPLE_OFF has succeeded; from now on tail progress/DONE must arrive.
  void expectFinalization({
    Duration timeout = const Duration(seconds: 12),
  }) {
    if (!_active) return;
    _expectingFinalization = true;
    _tailTimeout = timeout;
    _armTailWatchdog();
    _log('LIVE 尾包看门狗已启动（${timeout.inSeconds}s）');
  }

  Future<void> cancel() async {
    if (!_active) return;
    await _fail(TransferCancelledException());
  }

  Future<void> dispose() async {
    _log = (_) {};
    _onProgress = null;
    if (_active) {
      await cancel();
    } else {
      await _closeWatchers();
      await _closeOutput();
    }
  }

  Future<void> _handlePacket(List<int> data) async {
    if (!_active) return;

    switch (data[0]) {
      case BleProtocol.opLiveStart:
        await _handleStart(data);
        return;
      case BleProtocol.opLiveData:
        await _controller?.waitIfPaused();
        await _handleData(data);
        return;
      case BleProtocol.opLiveFinal:
        await _handleFinal(data);
        return;
      case BleProtocol.opLiveAbort:
        final reason = data.length > 1 ? data[1] : 0;
        throw StateError('板端终止 LIVE 同步，原因=$reason');
    }
  }

  Future<void> _handleStart(List<int> data) async {
    if (_output != null) return;
    final start = BleProtocol.parseLiveStart(data);
    if (start == null) {
      throw StateError('无效 LIVE START');
    }

    _filename = _safeFilename(start.filename);
    await _openOutput(_filename!);
    await _sendAck(force: true);
    _started = true;
    if (_startWaiter?.isCompleted == false) {
      _startWaiter!.complete();
    }
    _log('LIVE START: $_filename，手机开始写入 .part');
  }

  Future<void> _handleData(List<int> data) async {
    final frame = BleProtocol.parseLiveData(data);
    if (frame == null) {
      _log('LIVE DATA CRC/长度错误，请求从 seq=$_nextSeq 重传');
      await _sendAck(force: true);
      return;
    }
    if (_output == null) {
      throw StateError('LIVE DATA 早于 START');
    }

    if (frame.seq < _nextSeq && frame.offset < _received) {
      await _sendAck(force: true);
      return;
    }
    if (frame.seq != _nextSeq || frame.offset != _received) {
      _log(
        'LIVE DATA 跳号：期望 seq=$_nextSeq offset=$_received，'
        '收到 seq=${frame.seq} offset=${frame.offset}',
      );
      await _sendAck(force: true);
      return;
    }

    await _output!.writeFrom(frame.chunk);
    _crc32 = BleProtocol.crc32Update(_crc32, frame.chunk);
    _received += frame.chunk.length;
    _nextSeq++;
    _armTailWatchdog();
    _reportProgress();

    final atWindow = _nextSeq % BleProtocol.txFcWindowPkts == 0;
    final atFinal = _finalSize != null && _received >= _finalSize!;
    if (atWindow || atFinal) {
      await _sendAck(force: true);
    }
  }

  Future<void> _handleFinal(List<int> data) async {
    final finalInfo = BleProtocol.parseLiveFinal(data);
    if (finalInfo == null) {
      throw StateError('无效 LIVE FINAL');
    }

    if (_finalSize != null &&
        (_finalSize != finalInfo.size || _finalCrc32 != finalInfo.crc32)) {
      throw StateError(
        'LIVE 最终元数据变化: '
        '$_finalSize/0x${_finalCrc32?.toRadixString(16)} -> '
        '${finalInfo.size}/0x${finalInfo.crc32.toRadixString(16)}',
      );
    }
    _finalSize = finalInfo.size;
    _finalCrc32 = finalInfo.crc32;
    if (_received > finalInfo.size) {
      throw StateError(
        'LIVE 接收超过最终大小: $_received > ${finalInfo.size}',
      );
    }

    if (!finalInfo.done) {
      _log(
        '板端文件已封口：${finalInfo.size} B，'
        '正在补齐 ${finalInfo.size - _received} B',
      );
      await _sendAck(force: true);
      _armTailWatchdog();
      _reportProgress(force: true);
      return;
    }

    if (_received != finalInfo.size) {
      _log('LIVE DONE 提前到达，请求从 $_received B 继续');
      await _sendAck(force: true);
      return;
    }
    if (_crc32 != finalInfo.crc32) {
      throw StateError(
        'LIVE CRC32 不符: got=0x${_crc32.toRadixString(16)} '
        'expected=0x${finalInfo.crc32.toRadixString(16)}',
      );
    }

    // This ACK confirms receipt of DONE itself, so the board may close LIVE.
    await _sendAck(force: true);
    await _complete();
  }

  Future<void> _complete() async {
    if (_finishing) return;
    _finishing = true;
    final output = _output;
    final partPath = _partPath;
    final finalPath = _finalPath;
    final name = _filename;
    if (output == null ||
        partPath == null ||
        finalPath == null ||
        name == null) {
      throw StateError('LIVE 输出文件状态不完整');
    }

    await output.flush();
    await output.close();
    _output = null;

    final finalFile = File(finalPath);
    if (await finalFile.exists()) {
      await finalFile.delete();
    }
    await File(partPath).rename(finalPath);
    final actualSize = await finalFile.length();
    if (actualSize != _received) {
      throw StateError('LIVE 本地文件大小错误: $actualSize != $_received');
    }

    final elapsed =
        DateTime.now().difference(_startedAt!).inMilliseconds / 1000.0;
    final rate = elapsed > 0 ? _received / elapsed : 0.0;
    final result = LiveSyncResult(
      filename: name,
      localPath: finalPath,
      displayPath: _displayPath ?? finalPath,
      size: _received,
      crc32: _crc32,
      bytesPerSec: rate,
    );

    _active = false;
    await _closeWatchers();
    _reportProgress(force: true);
    _log(
      'LIVE 同步完成：${result.displayPath} · ${result.size} B · '
      'CRC32=0x${result.crc32.toRadixString(16).padLeft(8, '0')}',
    );
    if (_doneWaiter?.isCompleted == false) {
      _doneWaiter!.complete(result);
    }
  }

  Future<void> _openOutput(String filename) async {
    final candidates = Platform.isAndroid
        ? <({String dir, String display})>[
            (
              dir: '/storage/emulated/0/Download/TestBLE',
              display: '下载/TestBLE',
            ),
            (dir: '/sdcard/Download/TestBLE', display: '下载/TestBLE'),
          ]
        : <({String dir, String display})>[
            (
              dir: '${Directory.systemTemp.path}/TestBLE',
              display: '${Directory.systemTemp.path}/TestBLE',
            ),
          ];

    Object? lastError;
    for (final candidate in candidates) {
      try {
        final dir = Directory(candidate.dir);
        await dir.create(recursive: true);
        final finalPath = '${dir.path}/$filename';
        final partPath = '$finalPath.part';
        _output = await File(partPath).open(mode: FileMode.write);
        _partPath = partPath;
        _finalPath = finalPath;
        _displayPath = '${candidate.display}/$filename';
        return;
      } catch (e) {
        lastError = e;
      }
    }
    throw StateError('无法创建 LIVE 下载文件: $lastError');
  }

  Future<void> _resetStartAttempt() async {
    await _closeOutput();
    _filename = null;
    _partPath = null;
    _finalPath = null;
    _displayPath = null;
    _startWaiter = Completer<void>();
    _nextProgressBytes = 0;
    _nextSeq = 0;
    _received = 0;
    _crc32 = 0;
    _finalSize = null;
    _finalCrc32 = null;
    _started = false;
    _finishing = false;
  }

  String _safeFilename(String input) {
    final name = input.split(RegExp(r'[/\\]')).last;
    if (name.isEmpty || name == '.' || name == '..') {
      throw StateError('无效 LIVE 文件名: $input');
    }
    return name;
  }

  Future<void> _sendAck({required bool force}) async {
    if (!_active || _char == null) return;
    if (!force && _nextSeq % BleProtocol.txFcWindowPkts != 0) return;
    await BleTransferService.writeProtocol(
      _char!,
      BleProtocol.buildLiveAck(_nextSeq, _received),
      (_) {},
    );
  }

  Future<void> _write(List<int> data, String label) async {
    final char = _char;
    if (char == null) throw StateError('LIVE GATT 未绑定');
    await BleTransferService.writeProtocol(
      char,
      data,
      _log,
      label: label,
    );
  }

  Future<void> _abortBoardQuiet() async {
    final char = _char;
    if (char == null) return;
    try {
      await BleTransferService.writeProtocol(
        char,
        BleProtocol.buildLiveAbort(),
        (_) {},
      ).timeout(const Duration(seconds: 2));
    } catch (_) {}
  }

  void _reportProgress({bool force = false}) {
    final callback = _onProgress;
    final started = _startedAt;
    if (callback == null || started == null) return;
    final now = DateTime.now();
    final dueByBytes = _received >= _nextProgressBytes;
    final dueByTime = _lastProgressAt == null ||
        now.difference(_lastProgressAt!) >= const Duration(seconds: 1);
    if (!force && !dueByBytes && !dueByTime) return;

    final seconds = now.difference(started).inMilliseconds / 1000.0;
    callback(_received, _finalSize, seconds > 0 ? _received / seconds : 0);
    _lastProgressAt = now;
    _nextProgressBytes = _received + 16 * 1024;
  }

  Future<void> _closeOutput() async {
    final output = _output;
    _output = null;
    if (output != null) {
      try {
        await output.flush();
      } catch (_) {}
      try {
        await output.close();
      } catch (_) {}
    }
  }

  void _armTailWatchdog() {
    if (!_active || !_expectingFinalization) return;
    _tailWatchdog?.cancel();
    _tailWatchdog = Timer(_tailTimeout, () {
      if (_active && _expectingFinalization) {
        unawaited(
          _fail(
            TimeoutException(
              'LIVE 尾包 ${_tailTimeout.inSeconds}s 无进展 '
              '(received=$_received final=${_finalSize ?? "?"})',
            ),
          ),
        );
      }
    });
  }

  Future<void> _closeWatchers() async {
    _tailWatchdog?.cancel();
    _tailWatchdog = null;
    _expectingFinalization = false;
    final sub = _connectionSub;
    _connectionSub = null;
    if (sub != null) {
      await sub.cancel();
    }
  }

  Future<void> _fail(Object error) async {
    if (_failing || (!_active && _doneWaiter?.isCompleted == true)) return;
    _failing = true;
    final startWasCompleted = _startWaiter?.isCompleted == true;
    _active = false;
    await _abortBoardQuiet();
    await _closeWatchers();
    await _closeOutput();
    if (!startWasCompleted && _startWaiter?.isCompleted == false) {
      _startWaiter!.completeError(error);
    } else if (_doneWaiter?.isCompleted == false) {
      _doneWaiter!.completeError(error);
    }
    _log('LIVE 同步失败: $error');
    _failing = false;
  }
}
