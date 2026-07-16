// ArpPresetBrowser.tsx — browse the 238-preset arp library by genre and apply one.
//
// A genre chip row (colored per category) picks a category; the list below shows that
// genre's presets (name + description). Tapping a preset calls onApply — the parent runs
// applyArpPreset() to push it to the device and reflect it in the arp UI. Controlled only
// by `activeId` (which preset is currently applied, for highlight); category selection is
// local transient state.

import React, { useState } from 'react';
import { View, Text, Pressable, ScrollView, StyleSheet } from 'react-native';
import { ArpPreset, ARP_CATEGORIES, presetsByCategory, presetCount } from '../arpLibrary';

const K = {
  card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e',
  accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d',
};

export type ArpPresetBrowserProps = {
  onApply: (preset: ArpPreset) => void;
  activeId?: string;
};

export default function ArpPresetBrowser({ onApply, activeId }: ArpPresetBrowserProps) {
  const [cat, setCat] = useState<string>(ARP_CATEGORIES[0]?.key ?? 'classic');
  const presets = presetsByCategory(cat);

  return (
    <View style={b.wrap}>
      {/* Genre chips — each tinted with its category color. */}
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={b.catRow}>
        {ARP_CATEGORIES.map(c => {
          const on = c.key === cat;
          return (
            <Pressable key={c.key} onPress={() => setCat(c.key)}
              style={[b.catChip, on && { backgroundColor: c.color + '33', borderColor: c.color }]}>
              <View style={[b.dot, { backgroundColor: c.color }]} />
              <Text style={[b.catTxt, on && { color: K.text }]}>{c.label}</Text>
              <Text style={b.catCount}>{presetCount(c.key)}</Text>
            </Pressable>
          );
        })}
      </ScrollView>

      {/* Presets in the selected genre. */}
      <ScrollView style={b.list} nestedScrollEnabled>
        {presets.map(p => {
          const on = p.id === activeId;
          return (
            <Pressable key={p.id} onPress={() => onApply(p)} style={[b.item, on && b.itemOn]}>
              <Text style={b.itemName} numberOfLines={1}>{on ? '● ' : ''}{p.name}</Text>
              <Text style={b.itemDesc} numberOfLines={2}>{p.description}</Text>
            </Pressable>
          );
        })}
        {presets.length === 0 && <Text style={b.empty}>No presets in this genre.</Text>}
      </ScrollView>
    </View>
  );
}

const b = StyleSheet.create({
  wrap: { gap: 8, marginTop: 4 },
  catRow: { gap: 6, paddingVertical: 2, paddingRight: 8 },
  catChip: { flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: K.chip, borderWidth: 1, borderColor: K.border, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 14 },
  dot: { width: 8, height: 8, borderRadius: 4 },
  catTxt: { color: K.muted, fontSize: 13, fontWeight: '600' },
  catCount: { color: K.muted, fontSize: 11, opacity: 0.8 },
  list: { maxHeight: 260, borderWidth: 1, borderColor: K.border, borderRadius: 8 },
  item: { paddingHorizontal: 12, paddingVertical: 10, borderBottomWidth: 1, borderBottomColor: K.border },
  itemOn: { backgroundColor: K.sel },
  itemName: { color: K.text, fontSize: 14, fontWeight: '700' },
  itemDesc: { color: K.muted, fontSize: 12, marginTop: 2, lineHeight: 16 },
  empty: { color: K.muted, fontSize: 13, padding: 12 },
});
