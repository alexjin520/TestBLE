import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_transfer.dart';
import 'protocol.dart';

typedef LogFn = void Function(String msg);

class FnirsBleService {
  StreamSubscription<List<int>>? _notifySub;
  BluetoothCharacteristic? _char;
  String? _linkedRemoteId;
  final _rspWaiters = <int, Completer<FnirsRsp>>{};

  Future<void> resetLink() async {
    await _notifySub?.cancel();
    _notifySub = null;
    _char = null;
    _linkedRemoteId = null;
  }

  Future<BluetoothCharacteristic> _discoverChr(
      BluetoothDevice device, LogFn log) async {
    final services = await BleTransferService.discoverServicesWithRetry(
      device,
      log,
      remoteId: device.remoteId.str,
      preferredDevice: device,
    );
    final chr = BleTransferService.findXferChar(services);
    if (chr == null) {
      throw StateError('未找到特征 ${BleProtocol.chrUuid}');
    }
    _char = chr;
    _linkedRemoteId = device.remoteId.str;
    return chr;
  }

  bool _needsRelink(BluetoothDevice device) {
    return _char == null || _linkedRemoteId != device.remoteId.str;
  }

  static bool _isPrimaryServiceError(Object e) {
    return e.toString().contains('primary service not found');
  }

  Future<FnirsRsp> _sendAndWait(
    BluetoothDevice device,
    Uint8List cmd,
    int echoOp,
    LogFn log, {
    Duration timeout = const Duration(seconds: 5),
  }) async {
    for (var attempt = 1; attempt <= 2; attempt++) {
      if (_needsRelink(device)) {
        await resetLink();
        await _discoverChr(device, log);
      }

      final chr = _char!;
      final c = Completer<FnirsRsp>();
      _rspWaiters[echoOp] = c;

      try {
        await _ensureNotify(device, chr, log);
        await _writeFnirsCmd(device, chr, cmd, echoOp);
        return await c.future.timeout(timeout, onTimeout: () {
          _rspWaiters.remove(echoOp);
          throw TimeoutException('FNIRS rsp 0x${echoOp.toRadixString(16)}');
        });
      } catch (e) {
        _rspWaiters.remove(echoOp);
        if (attempt < 2 && _isPrimaryServiceError(e)) {
          log('GATT 缓存失效，重新 discover 后重试…');
          await resetLink();
          continue;
        }
        rethrow;
      }
    }
    throw StateError('FNIRS 命令发送失败');
  }

  /// Board xfer char is WWR-only; use negotiated MTU for payloads > 20 B.
  Future<void> _writeFnirsCmd(
    BluetoothDevice device,
    BluetoothCharacteristic chr,
    Uint8List cmd,
    int echoOp,
  ) async {
    await BleTransferService.writeProtocol(
      chr,
      cmd,
      (_) {},
      label: 'FNIRS 0x${echoOp.toRadixString(16)}',
      mtu: device.mtuNow,
    );
  }

  Future<void> prepareLink(BluetoothDevice device, LogFn log) async {
    await resetLink();
    await _discoverChr(device, log);
    await BleTransferService.gattRun(
      () => BleTransferService.negotiateMtuForDevice(
        device,
        log,
        trustPhoneMtu: true,
      ),
    );
    await BleTransferService.enableNotifyWithRetry(_char!, log);
    if (_notifySub != null) return;
    _notifySub = _char!.onValueReceived.listen(_onNotify);
    device.cancelWhenDisconnected(_notifySub!);
  }

  Future<void> _ensureNotify(
      BluetoothDevice device, BluetoothCharacteristic chr, LogFn log) async {
    if (_notifySub != null) return;
    await BleTransferService.enableNotifyWithRetry(chr, log);
    _notifySub = chr.onValueReceived.listen(_onNotify);
    device.cancelWhenDisconnected(_notifySub!);
  }

  void _onNotify(List<int> data) {
    final rsp = BleProtocol.parseFnirsRsp(data);
    if (rsp == null) return;
    final w = _rspWaiters.remove(rsp.echoOp);
    if (w != null && !w.isCompleted) {
      w.complete(rsp);
    }
  }

  Future<List<bool>> scanNodes(BluetoothDevice device, LogFn log) async {
    try {
      final rsp = await _sendAndWait(
        device,
        BleProtocol.buildFnirsScan(),
        BleProtocol.opFnirsScan,
        log,
        timeout: const Duration(seconds: 10),
      );
      if (!rsp.ok) {
        throw StateError('scan status=${rsp.status}');
      }
      return BleProtocol.parseFnirsScanAlive(rsp.payload);
    } on TimeoutException {
      throw StateError('扫描超时：请确认已刷新版 system.ext2 且 my-fnirs 在运行');
    }
  }

  Future<void> sampleOn(BluetoothDevice device, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsSampleOn(),
      BleProtocol.opFnirsSampleOn,
      log,
    );
    if (!rsp.ok) throw StateError('sample on failed');
  }

  Future<void> sampleOff(BluetoothDevice device, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsSampleOff(),
      BleProtocol.opFnirsSampleOff,
      log,
      timeout: const Duration(seconds: 15),
    );
    if (!rsp.ok) {
      throw StateError('sample off/file finalize failed');
    }
  }

  Future<void> setGain(BluetoothDevice device, int gain, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsGain(gain),
      BleProtocol.opFnirsGain,
      log,
    );
    if (!rsp.ok) throw StateError('gain failed');
  }

  Future<void> setLedArray(
      BluetoothDevice device, List<List<int>> entries, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsLedArray(entries),
      BleProtocol.opFnirsLedArray,
      log,
    );
    if (!rsp.ok) throw StateError('led array failed');
  }

  Future<void> setStreamChannels(
      BluetoothDevice device, List<FnirsChannel> channels, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsStreamCh(channels),
      BleProtocol.opFnirsStreamCh,
      log,
    );
    if (!rsp.ok) throw StateError('stream ch failed');
  }

  Future<String> queryHangzhouPath(BluetoothDevice device, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsHangzhouReq(),
      BleProtocol.opFnirsHangzhou,
      log,
    );
    if (!rsp.ok) throw StateError('no Hangzhou file');
    final path = BleProtocol.parseFnirsHangzhouPath(rsp.payload);
    if (path == null || path.isEmpty) throw StateError('empty path');
    return path;
  }

  Future<FnirsRecordStat> queryRecordStat(
      BluetoothDevice device, LogFn log) async {
    final rsp = await _sendAndWait(
      device,
      BleProtocol.buildFnirsRecordStatReq(),
      BleProtocol.opFnirsRecordStat,
      log,
      timeout: const Duration(seconds: 3),
    );
    if (!rsp.ok) throw StateError('record stat unavailable');
    final stat = BleProtocol.parseFnirsRecordStat(rsp.payload);
    if (stat == null) throw StateError('bad record stat payload');
    return stat;
  }

  Future<void> dispose() async {
    await resetLink();
    for (final c in _rspWaiters.values) {
      if (!c.isCompleted) c.completeError(StateError('disposed'));
    }
    _rspWaiters.clear();
  }
}
