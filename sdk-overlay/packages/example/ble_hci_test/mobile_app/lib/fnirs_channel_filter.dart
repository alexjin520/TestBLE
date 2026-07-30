import 'package:flutter/material.dart';

import 'protocol.dart';

/// Hub-style channel filter: affects **display** only (and BLE preview subset).
class FnirsChannelFilterDialog extends StatefulWidget {
  const FnirsChannelFilterDialog({
    super.key,
    required this.initialDisplay,
    required this.maxDisplay,
    required this.maxBlePreview,
  });

  final List<FnirsChannel> initialDisplay;
  final int maxDisplay;
  final int maxBlePreview;

  @override
  State<FnirsChannelFilterDialog> createState() =>
      _FnirsChannelFilterDialogState();
}

class FnirsChannelFilterResult {
  const FnirsChannelFilterResult({
    required this.displayChannels,
    required this.blePreviewChannels,
  });

  final List<FnirsChannel> displayChannels;
  final List<FnirsChannel> blePreviewChannels;
}

class _FnirsChannelFilterDialogState extends State<FnirsChannelFilterDialog> {
  late int _srcNode;
  late int _ledId;
  late int _detNode;
  late int _sensorId;
  late List<FnirsChannel> _display;

  @override
  void initState() {
    super.initState();
    _display = List.of(widget.initialDisplay);
    final first = _display.isNotEmpty
        ? _display.first
        : const FnirsChannel(srcNode: 10, ledId: 2, detNode: 2);
    _srcNode = first.srcNode;
    _ledId = first.ledId;
    _detNode = first.detNode;
    _sensorId = first.detId;
  }

  FnirsChannel get _current => FnirsChannel(
        srcNode: _srcNode,
        ledId: _ledId,
        detNode: _detNode,
        detId: _sensorId,
      );

  void _addDisplay() {
    final ch = _current;
    if (_display.length >= widget.maxDisplay) return;
    if (_display.any((c) => FnirsChannelExt.same(c, ch))) return;
    setState(() => _display.add(ch));
  }

  void _removeDisplay(int i) {
    setState(() => _display.removeAt(i));
  }

  List<FnirsChannel> _buildBlePreview() {
    // BLE preview carries display channels first, then pad with defaults.
    final out = <FnirsChannel>[];
    final seen = <String>{};

    void add(FnirsChannel c) {
      final k = '${c.srcNode}-${c.ledId}-${c.detNode}-${c.detId}';
      if (seen.contains(k) || out.length >= widget.maxBlePreview) return;
      seen.add(k);
      out.add(c);
    }

    for (final c in _display) {
      add(c);
    }
    for (final c in FnirsChannelExt.defaultPreviewSet) {
      add(c);
    }
    return out;
  }

  @override
  Widget build(BuildContext context) {
    final lightSources = <String>[];
    for (var n = 1; n <= 12; n++) {
      for (var l = 1; l <= 3; l++) {
        lightSources.add('节点${n}_光源$l');
      }
    }
    final sensors = <String>[];
    for (var n = 1; n <= 12; n++) {
      for (var s = 1; s <= 4; s++) {
        sensors.add('节点${n}_传感器$s');
      }
    }

    return AlertDialog(
      title: const Text('通道筛选'),
      content: SizedBox(
        width: 420,
        height: 420,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const Text(
              '仅改变下方波形显示（及 BLE 预览子集）。'
              '板子按「发光序列」全通道采集并写入 Hangzhou.bin；'
              '此处仅筛选 App 显示与 BLE 预览通道。',
              style: TextStyle(fontSize: 11, color: Colors.black54),
            ),
            const SizedBox(height: 8),
            Expanded(
              child: Row(
                children: [
                  Expanded(
                    child: _pickerCol(
                      '光源',
                      lightSources,
                      (_srcNode - 1) * 3 + (_ledId - 1),
                      (i) {
                        setState(() {
                          _srcNode = i ~/ 3 + 1;
                          _ledId = i % 3 + 1;
                        });
                      },
                    ),
                  ),
                  Expanded(
                    child: _pickerCol(
                      '传感器',
                      sensors,
                      (_detNode - 1) * 4 + (_sensorId - 1),
                      (i) {
                        setState(() {
                          _detNode = i ~/ 4 + 1;
                          _sensorId = i % 4 + 1;
                        });
                      },
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 8),
            Text('显示通道 (${_display.length}/${widget.maxDisplay})',
                style: const TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 4),
            Wrap(
              spacing: 6,
              children: [
                for (var i = 0; i < _display.length; i++)
                  InputChip(
                    label: Text(_display[i].label,
                        style: const TextStyle(fontSize: 11)),
                    onDeleted: () => _removeDisplay(i),
                  ),
              ],
            ),
            const SizedBox(height: 6),
            OutlinedButton.icon(
              onPressed:
                  _display.length >= widget.maxDisplay ? null : _addDisplay,
              icon: const Icon(Icons.add, size: 18),
              label: Text('添加: ${_current.label}'),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: _display.isEmpty
              ? null
              : () {
                  Navigator.pop(
                    context,
                    FnirsChannelFilterResult(
                      displayChannels: List.of(_display),
                      blePreviewChannels: _buildBlePreview(),
                    ),
                  );
                },
          child: const Text('确定'),
        ),
      ],
    );
  }

  Widget _pickerCol(
    String title,
    List<String> items,
    int selected,
    ValueChanged<int> onSelect,
  ) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
        const SizedBox(height: 4),
        Expanded(
          child: DecoratedBox(
            decoration: BoxDecoration(
              border: Border.all(color: Colors.grey.shade400),
              borderRadius: BorderRadius.circular(4),
            ),
            child: ListView.builder(
              itemCount: items.length,
              itemBuilder: (context, i) {
                final on = i == selected;
                return ListTile(
                  dense: true,
                  title: Text(items[i], style: const TextStyle(fontSize: 12)),
                  selected: on,
                  onTap: () => onSelect(i),
                );
              },
            ),
          ),
        ),
      ],
    );
  }
}

extension FnirsChannelExt on FnirsChannel {
  static bool same(FnirsChannel a, FnirsChannel b) =>
      a.srcNode == b.srcNode &&
      a.ledId == b.ledId &&
      a.detNode == b.detNode &&
      a.detId == b.detId;

  /// Default BLE preview (4 ch fits 20-byte WWR: 2+12=14 B).
  static List<FnirsChannel> get defaultPreviewSet =>
      List.of(defaultDisplaySet);

  static List<FnirsChannel> get defaultDisplaySet => [
        const FnirsChannel(srcNode: 10, ledId: 2, detNode: 2),
        const FnirsChannel(srcNode: 10, ledId: 2, detNode: 3),
        const FnirsChannel(srcNode: 10, ledId: 2, detNode: 4),
        const FnirsChannel(srcNode: 10, ledId: 2, detNode: 5),
      ];
}
