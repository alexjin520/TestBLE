import 'package:flutter/material.dart';

import 'protocol.dart';

/// Visual editor for board LED illumination sequence (BLE 0x44 / FUSB 0x0006).
class FnirsLedArrayDialog extends StatefulWidget {
  const FnirsLedArrayDialog({super.key, required this.initialEntries});

  final List<List<int>> initialEntries;

  @override
  State<FnirsLedArrayDialog> createState() => _FnirsLedArrayDialogState();
}

class _FnirsLedArrayDialogState extends State<FnirsLedArrayDialog> {
  late List<List<int>> _steps;
  late int _power735;
  late int _power850;
  int _addNode = 1;
  int _addLed = 1;

  @override
  void initState() {
    super.initState();
    _steps = BleProtocol.cloneLedArrayEntries(widget.initialEntries);
    _power735 = _steps.isNotEmpty ? _steps.first[2] : BleProtocol.defaultLedPower735;
    _power850 = _steps.isNotEmpty ? _steps.first[3] : BleProtocol.defaultLedPower850;
  }

  void _applyUniformPower() {
    _steps = BleProtocol.ledArrayWithUniformPower(_steps, _power735, _power850);
  }

  void _loadPreset(List<List<int>> preset) {
    setState(() {
      _steps = BleProtocol.cloneLedArrayEntries(preset);
      if (_steps.isNotEmpty) {
        _power735 = _steps.first[2];
        _power850 = _steps.first[3];
      }
    });
  }

  void _addStep() {
    setState(() {
      _steps.add([_addNode, _addLed, _power735, _power850]);
    });
  }

  void _removeStep(int index) {
    setState(() => _steps.removeAt(index));
  }

  void _moveStep(int index, int delta) {
    final j = index + delta;
    if (j < 0 || j >= _steps.length) return;
    setState(() {
      final tmp = _steps[index];
      _steps[index] = _steps[j];
      _steps[j] = tmp;
    });
  }

  void _updateStep(int index, {int? node, int? led}) {
    setState(() {
      final e = _steps[index];
      _steps[index] = [
        node ?? e[0],
        led ?? e[1],
        _power735,
        _power850,
      ];
    });
  }

  void _confirm() {
    if (_steps.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请至少添加一步发光')),
      );
      return;
    }
    _applyUniformPower();
    Navigator.pop(context, BleProtocol.cloneLedArrayEntries(_steps));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('发光序列'),
      content: SizedBox(
        width: double.maxFinite,
        child: SingleChildScrollView(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text(
                '按顺序定义每一步「哪个节点、哪盏灯」亮 735/850。'
                '在线节点都会作为探测器收光。',
                style: TextStyle(fontSize: 11, color: Colors.black54),
              ),
              const SizedBox(height: 10),
              Wrap(
                spacing: 6,
                runSpacing: 6,
                children: [
                  ActionChip(
                    label: const Text('默认 10-12'),
                    onPressed: () =>
                        _loadPreset(BleProtocol.defaultLedArrayEntries),
                  ),
                  ActionChip(
                    label: const Text('测试 1-2 L1L2'),
                    onPressed: () =>
                        _loadPreset(BleProtocol.testLedArrayNode12Entries),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              Text(
                BleProtocol.ledArraySummary(_steps),
                style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13),
              ),
              const SizedBox(height: 8),
              if (_steps.isEmpty)
                const Padding(
                  padding: EdgeInsets.symmetric(vertical: 12),
                  child: Text('暂无步骤，请添加或选择预设',
                      style: TextStyle(color: Colors.black45)),
                )
              else
                ...List.generate(_steps.length, (i) {
                  final e = _steps[i];
                  return Card(
                    margin: const EdgeInsets.only(bottom: 6),
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(8, 4, 4, 4),
                      child: Row(
                        children: [
                          Text('${i + 1}',
                              style: const TextStyle(
                                  fontWeight: FontWeight.bold, fontSize: 12)),
                          const SizedBox(width: 6),
                          Expanded(
                            child: DropdownButtonFormField<int>(
                              isExpanded: true,
                              value: e[0],
                              decoration: const InputDecoration(
                                labelText: '节点',
                                isDense: true,
                                contentPadding: EdgeInsets.symmetric(
                                    horizontal: 8, vertical: 8),
                              ),
                              items: List.generate(
                                12,
                                (n) => DropdownMenuItem(
                                  value: n + 1,
                                  child: Text('节点 ${n + 1}',
                                      style: const TextStyle(fontSize: 13)),
                                ),
                              ),
                              onChanged: (v) {
                                if (v != null) _updateStep(i, node: v);
                              },
                            ),
                          ),
                          const SizedBox(width: 6),
                          Expanded(
                            child: DropdownButtonFormField<int>(
                              isExpanded: true,
                              value: e[1],
                              decoration: const InputDecoration(
                                labelText: '光源',
                                isDense: true,
                                contentPadding: EdgeInsets.symmetric(
                                    horizontal: 8, vertical: 8),
                              ),
                              items: const [
                                DropdownMenuItem(
                                    value: 1, child: Text('L1', style: TextStyle(fontSize: 13))),
                                DropdownMenuItem(
                                    value: 2, child: Text('L2', style: TextStyle(fontSize: 13))),
                                DropdownMenuItem(
                                    value: 3, child: Text('L3', style: TextStyle(fontSize: 13))),
                              ],
                              onChanged: (v) {
                                if (v != null) _updateStep(i, led: v);
                              },
                            ),
                          ),
                          Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              IconButton(
                                icon: const Icon(Icons.arrow_upward, size: 18),
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(
                                    minWidth: 32, minHeight: 28),
                                onPressed:
                                    i > 0 ? () => _moveStep(i, -1) : null,
                              ),
                              IconButton(
                                icon: const Icon(Icons.delete_outline, size: 18),
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(
                                    minWidth: 32, minHeight: 28),
                                onPressed: () => _removeStep(i),
                              ),
                              IconButton(
                                icon: const Icon(Icons.arrow_downward, size: 18),
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(
                                    minWidth: 32, minHeight: 28),
                                onPressed: i < _steps.length - 1
                                    ? () => _moveStep(i, 1)
                                    : null,
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  );
                }),
              const Divider(height: 20),
              const Text('新增一步', style: TextStyle(fontWeight: FontWeight.w600)),
              const SizedBox(height: 6),
              Row(
                children: [
                  Expanded(
                    child: DropdownButtonFormField<int>(
                      value: _addNode,
                      decoration: const InputDecoration(
                        labelText: '节点',
                        isDense: true,
                      ),
                      items: List.generate(
                        12,
                        (n) => DropdownMenuItem(
                          value: n + 1,
                          child: Text('节点 ${n + 1}'),
                        ),
                      ),
                      onChanged: (v) {
                        if (v != null) setState(() => _addNode = v);
                      },
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: DropdownButtonFormField<int>(
                      value: _addLed,
                      decoration: const InputDecoration(
                        labelText: '光源',
                        isDense: true,
                      ),
                      items: const [
                        DropdownMenuItem(value: 1, child: Text('L1')),
                        DropdownMenuItem(value: 2, child: Text('L2')),
                        DropdownMenuItem(value: 3, child: Text('L3')),
                      ],
                      onChanged: (v) {
                        if (v != null) setState(() => _addLed = v);
                      },
                    ),
                  ),
                  IconButton.filledTonal(
                    onPressed: _addStep,
                    icon: const Icon(Icons.add),
                    tooltip: '添加',
                  ),
                ],
              ),
              const SizedBox(height: 8),
              Text('735 电流: 0x${_power735.toRadixString(16).toUpperCase().padLeft(2, '0')}'),
              Slider(
                min: 0,
                max: 63,
                divisions: 63,
                value: _power735.toDouble(),
                onChanged: (v) => setState(() {
                  _power735 = v.round();
                  _applyUniformPower();
                }),
              ),
              Text('850 电流: 0x${_power850.toRadixString(16).toUpperCase().padLeft(2, '0')}'),
              Slider(
                min: 0,
                max: 63,
                divisions: 63,
                value: _power850.toDouble(),
                onChanged: (v) => setState(() {
                  _power850 = v.round();
                  _applyUniformPower();
                }),
              ),
            ],
          ),
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('取消'),
        ),
        FilledButton(
          onPressed: _confirm,
          child: const Text('确定'),
        ),
      ],
    );
  }
}
