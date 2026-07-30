import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_transfer.dart';
import 'protocol.dart';

typedef LogFn = void Function(String msg);

/// Push phone wall-clock to board (TIME_OP_SET 0x35).
class BleTimeSync {
  static const _minEpoch = 1577836800; // 2020-01-01

  static Future<void> pushToCharacteristic(
    BluetoothCharacteristic char,
    LogFn log,
  ) async {
    final now = DateTime.now().toUtc();
    final unix = now.millisecondsSinceEpoch ~/ 1000;
    if (unix < _minEpoch) {
      log('跳过对时：手机时间无效');
      return;
    }
    final pkt = BleProtocol.buildTimeSet(unix);
    await BleTransferService.writeProtocol(char, pkt, log, label: 'TIME_SET');
    log(
      '已对时到板子: ${now.toIso8601String()} (unix=$unix)',
    );
  }
}
