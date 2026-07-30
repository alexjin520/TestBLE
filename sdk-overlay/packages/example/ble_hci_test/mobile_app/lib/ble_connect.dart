import 'dart:async';
import 'dart:io';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'protocol.dart';

typedef LogFn = void Function(String msg);

/// All name fields FlutterBluePlus may populate during scan.
Iterable<String> bleScanNameCandidates(ScanResult r) sync* {
  yield r.advertisementData.advName;
  yield r.device.platformName;
}

/// Name from ADV packet; on Android [BluetoothDevice.platformName] is often empty during scan.
String bleScanAdvertisedName(ScanResult r) {
  for (final raw in bleScanNameCandidates(r)) {
    final n = raw.trim();
    if (n.isNotEmpty) return n;
  }
  return '';
}

bool bleScanIsBoardByMac(ScanResult r) {
  final id = r.device.remoteId.str.toUpperCase();
  for (final prefix in BleProtocol.boardMacPrefixes) {
    if (id.startsWith(prefix.toUpperCase())) return true;
  }
  final hint = BleProtocol.defaultBoardMac.trim().toUpperCase();
  if (hint.isNotEmpty && id == hint) return true;
  return false;
}

bool bleScanIsTestBle(ScanResult r, {String? knownRemoteId}) {
  final id = r.device.remoteId.str.toUpperCase();
  if (knownRemoteId != null &&
      knownRemoteId.isNotEmpty &&
      id == knownRemoteId.toUpperCase()) {
    return true;
  }

  // Android often omits advName — MAC prefix is the reliable board signature.
  if (bleScanIsBoardByMac(r)) return true;

  final target = BleProtocol.defaultName.toLowerCase();
  for (final raw in bleScanNameCandidates(r)) {
    final n = raw.trim().toLowerCase();
    if (n == target || n.contains(target)) return true;
  }

  for (final u in r.advertisementData.serviceUuids) {
    final uuid = u.str.toLowerCase().replaceAll('-', '');
    if (uuid.contains('78563412')) return true;
  }

  return false;
}

/// Merge scan batches; keep strongest RSSI per remoteId.
void bleMergeScanBatch(Map<String, ScanResult> acc, List<ScanResult> batch) {
  for (final r in batch) {
    final id = r.device.remoteId.str.toUpperCase();
    final prev = acc[id];
    if (prev == null || r.rssi > prev.rssi) {
      acc[id] = r;
    }
  }
}

String bleScanResultLabel(ScanResult r) {
  final name = bleScanAdvertisedName(r);
  final displayName =
      name.isEmpty ? '(无广播名)' : name;
  final boardTag = bleScanIsBoardByMac(r) ? ' [板子]' : '';
  return '$displayName$boardTag\n${r.device.remoteId.str}  RSSI ${r.rssi}';
}

/// Request Android 12+ BLE permissions before scanning.
Future<bool> bleEnsureScanReady({LogFn? log}) async {
  if (await FlutterBluePlus.isSupported == false) {
    throw StateError('本机不支持 BLE');
  }

  if (Platform.isAndroid) {
    if (!await FlutterBluePlus.isOn) {
      log?.call('正在打开蓝牙…');
      await FlutterBluePlus.turnOn();
      await FlutterBluePlus.adapterState
          .firstWhere((s) => s == BluetoothAdapterState.on)
          .timeout(const Duration(seconds: 15));
    }
    log?.call(
      '若扫描为空，请到系统设置允许本 App「附近的设备/蓝牙」权限',
    );
  }

  return true;
}

/// Broad scan like nRF Connect — never pass [withRemoteIds] during discovery
/// (wrong saved MAC makes Android return zero results).
Future<void> bleStartTestBleScan({
  required Duration timeout,
  AndroidScanMode androidScanMode = AndroidScanMode.lowLatency,
  List<String>? withRemoteIds,
  bool discoveryOnly = false,
}) async {
  try {
    await FlutterBluePlus.stopScan();
  } catch (_) {}
  await Future<void>.delayed(const Duration(milliseconds: 250));

  if (!discoveryOnly &&
      withRemoteIds != null &&
      withRemoteIds.isNotEmpty) {
    await FlutterBluePlus.startScan(
      timeout: timeout,
      androidScanMode: androidScanMode,
      withRemoteIds: withRemoteIds,
    );
    return;
  }

  await FlutterBluePlus.startScan(
    timeout: timeout,
    androidScanMode: androidScanMode,
  );
}

/// Run a full discovery scan and return deduplicated results (strongest RSSI).
Future<List<ScanResult>> bleRunDiscoveryScan({
  Duration duration = const Duration(seconds: 12),
  LogFn? log,
}) async {
  final acc = <String, ScanResult>{};
  final sub = FlutterBluePlus.onScanResults.listen((results) {
    bleMergeScanBatch(acc, results);
  });

  log?.call('全量扫描 ${duration.inSeconds}s（无 MAC 过滤）…');
  await bleStartTestBleScan(
    timeout: duration,
    discoveryOnly: true,
  );
  await Future<void>.delayed(duration);
  try {
    await FlutterBluePlus.stopScan();
  } catch (_) {}
  await sub.cancel();

  bleMergeScanBatch(acc, FlutterBluePlus.lastScanResults);

  final all = acc.values.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
  final boardCount = all.where(bleScanIsBoardByMac).length;
  log?.call('扫描结束: 共 ${all.length} 个设备, 板子 MAC 候选 $boardCount 个');
  return all;
}

/// Android must connect to a device seen in a recent scan (not stale fromId).
class BleConnect {
  /// Tear down an Android GATT session completely before starting a new scan.
  /// `BluetoothDevice.disconnect()` may return before the platform callback has
  /// reached `disconnected`; scanning in that window commonly leaves the next
  /// connect attached to the old GATT client.
  static Future<void> disconnectAndWait(
    BluetoothDevice device, {
    LogFn? log,
    bool clearGattCache = false,
  }) async {
    try {
      await device.disconnect();
    } catch (e) {
      log?.call('断开请求返回: $e');
    }

    try {
      await device.connectionState
          .firstWhere((s) => s == BluetoothConnectionState.disconnected)
          .timeout(const Duration(seconds: 5));
    } catch (_) {
      // Some Android vendors do not emit the terminal callback after an
      // already-disconnected link. Continue with cache cleanup below.
    }

    // Android's hidden refresh() is only for a changed firmware GATT table.
    // Refreshing on every normal disconnect discards the valid raw-ATT
    // properties and is known to make the next discovery return empty props.
    if (clearGattCache && Platform.isAndroid) {
      try {
        await device.clearGattCache();
      } catch (_) {}
    }
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 800 : 250),
    );
  }

  /// Scan → pick device by [remoteId] → connect with retries. Returns live instance.
  ///
  /// Pass [preferredDevice] from a recent UI scan when possible — on Android,
  /// [BluetoothDevice.fromId] alone often fails to connect.
  static Future<BluetoothDevice> scanAndConnect({
    required String remoteId,
    required LogFn log,
    BluetoothDevice? preferredDevice,
    Duration scanTimeout = const Duration(seconds: 5),
    Duration connectTimeout = const Duration(seconds: 35),
    int maxAttempts = 3,
    int scanRounds = 1,
    bool disconnectAllFirst = true,
    bool requireFreshScan = false,
  }) async {
    if (preferredDevice != null && await isConnected(preferredDevice)) {
      log('设备已连接，直接复用 GATT');
      return preferredDevice;
    }

    if (disconnectAllFirst) {
      await _disconnectAll(log);
    } else {
      try {
        await FlutterBluePlus.stopScan();
      } catch (_) {}
    }

    BluetoothDevice? fresh;
    final rounds = scanRounds < 1 ? 1 : scanRounds;
    for (var round = 1; round <= rounds; round++) {
      if (round > 1) {
        log('扫描重试 $round/$rounds（${scanTimeout.inSeconds}s）…');
        await Future<void>.delayed(Duration(milliseconds: 800 * round));
      } else {
        log('全量扫描匹配 TestBLE（${scanTimeout.inSeconds}s）…');
      }

      fresh = null;
      final byName = <ScanResult>[];
      final target = remoteId.toUpperCase();
      final sub = FlutterBluePlus.onScanResults.listen((results) {
        for (final r in results) {
          final id = r.device.remoteId.str.toUpperCase();
          if (id == target) {
            fresh = r.device;
          }
          if (bleScanIsTestBle(r, knownRemoteId: remoteId)) {
            final exists =
                byName.any((x) => x.device.remoteId == r.device.remoteId);
            if (!exists) byName.add(r);
          }
        }
      });

      await bleStartTestBleScan(
        timeout: scanTimeout,
        discoveryOnly: true,
      );
      await Future<void>.delayed(scanTimeout);
      await FlutterBluePlus.stopScan();
      await sub.cancel();

      if (fresh != null) break;

      if (round == rounds && byName.isNotEmpty && preferredDevice == null) {
        byName.sort((a, b) => b.rssi.compareTo(a.rssi));
        fresh = byName.first.device;
        log(
          '未扫到 MAC=$remoteId，回退最强 TestBLE ${fresh!.remoteId} '
          '(RSSI=${byName.first.rssi})',
        );
        break;
      }
    }

    BluetoothDevice device;
    if (fresh != null) {
      device = fresh!;
      log('扫描确认设备在线 RSSI 可见');
    } else if (preferredDevice != null) {
      if (requireFreshScan) {
        log('${rounds} 轮扫描未见 $remoteId，仍尝试连接 UI 选中设备…');
      } else {
        log('扫描未见到 $remoteId，使用 UI 选中的设备连接');
      }
      device = preferredDevice;
    } else {
      device = BluetoothDevice.fromId(remoteId);
      log('扫描未见到 $remoteId，也未发现 TestBLE，仍尝试旧地址连接');
    }

    Object? lastErr;
    for (var attempt = 1; attempt <= maxAttempts; attempt++) {
      try {
        log('GATT 连接 $attempt/$maxAttempts (mtu:null, timeout ${connectTimeout.inSeconds}s)…');
        await device.connect(
          timeout: connectTimeout,
          autoConnect: false,
          mtu: null, // FBP Android default mtu=512 → requestMtu; board raw ATT never answers
        );
        await device.connectionState
            .firstWhere((s) => s == BluetoothConnectionState.connected)
            .timeout(connectTimeout);
        log('已连接');
        await waitGattStable(device, log);

        await Future<void>.delayed(
          Duration(milliseconds: Platform.isAndroid ? 1200 : 700),
        );
        return device;
      } catch (e) {
        lastErr = e;
        log('连接失败: $e');
        await disconnectAndWait(device, log: log);
        if (attempt < maxAttempts) {
          log('2 秒后重试（若仍失败请板子执行 start-my-server -U）');
          await Future<void>.delayed(const Duration(seconds: 2));
        }
      }
    }
    throw StateError(
      'GATT 连接超时。请确认：\n'
      '1) 板子已 start-my-server 且串口 Waiting for connections\n'
      '2) 手机刚点过「扫描」\n'
      '3) PC 未占用连接\n'
      '4) 关闭再开手机蓝牙后重试\n'
      '($lastErr)',
    );
  }

  static Future<void> _disconnectAll(LogFn log) async {
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    for (final d in FlutterBluePlus.connectedDevices) {
      log('断开旧连接: ${d.remoteId}');
      await disconnectAndWait(d, log: log);
    }
    await Future<void>.delayed(const Duration(milliseconds: 400));
  }

  /// True if [device] is already in connected state.
  static Future<bool> isConnected(BluetoothDevice device) async {
    try {
      final state = await device.connectionState
          .where((s) =>
              s == BluetoothConnectionState.connected ||
              s == BluetoothConnectionState.disconnected)
          .first
          .timeout(const Duration(seconds: 2));
      return state == BluetoothConnectionState.connected;
    } catch (_) {
      return false;
    }
  }

  /// After discover/notify — optional Android throughput tweak.
  static Future<void> requestHighPriorityIfAndroid(
    BluetoothDevice device,
    LogFn log,
  ) async {
    if (!Platform.isAndroid) return;
    try {
      await device.requestConnectionPriority(
        connectionPriorityRequest: ConnectionPriority.high,
      );
      log('已请求高优先级连接 (Android)');
    } catch (e) {
      log('requestConnectionPriority 忽略: $e');
    }
  }

  static bool isDisconnectError(Object e) {
    final msg = e.toString().toLowerCase();
    return msg.contains('disconnected') ||
        msg.contains('not connected') ||
        msg.contains('fbp-code: 6');
  }

  /// Ensure the link stays up briefly before discover / notify.
  static Future<void> waitGattStable(
    BluetoothDevice device,
    LogFn log, {
    int checks = 3,
    Duration step = const Duration(milliseconds: 400),
  }) async {
    for (var i = 0; i < checks; i++) {
      if (!await isConnected(device)) {
        throw StateError('连接不稳定（discover 前已断开）');
      }
      if (i + 1 < checks) {
        await Future<void>.delayed(step);
      }
    }
  }

  /// Reuse an existing GATT link when sending multiple files in a row.
  static Future<BluetoothDevice> connectForTransfer({
    required String remoteId,
    required LogFn log,
    BluetoothDevice? preferredDevice,
    bool reuseIfConnected = false,
    bool forceReconnect = false,
    Duration scanTimeout = const Duration(seconds: 5),
    Duration connectTimeout = const Duration(seconds: 35),
    int maxAttempts = 3,
    bool disconnectAllFirst = false,
  }) async {
    if (forceReconnect) {
      log('GATT 忙/异常，断开并重新连接…');
      if (preferredDevice != null) {
        try {
          if (await isConnected(preferredDevice)) {
            await preferredDevice.disconnect();
          }
        } catch (_) {}
      }
      await Future<void>.delayed(const Duration(milliseconds: 1000));
      return scanAndConnect(
        remoteId: remoteId,
        preferredDevice: preferredDevice,
        log: log,
        scanTimeout: scanTimeout,
        connectTimeout: connectTimeout,
        maxAttempts: maxAttempts,
        scanRounds: 3,
        disconnectAllFirst: true,
      );
    }
    if (reuseIfConnected && preferredDevice != null) {
      if (await isConnected(preferredDevice)) {
        log('复用当前 GATT 连接（连续传文件）');
        return preferredDevice;
      }
      log('连接已断开，重新扫描连接…');
    }
    return scanAndConnect(
      remoteId: remoteId,
      preferredDevice: preferredDevice,
      log: log,
      scanTimeout: scanTimeout,
      connectTimeout: connectTimeout,
      maxAttempts: maxAttempts,
      scanRounds: 3,
      disconnectAllFirst: disconnectAllFirst,
    );
  }
}
