// WifiQr.tsx — a Wi-Fi join QR code drawn with plain Views (no SVG/native dependency, so it ships in an
// over-the-air update). Phone cameras on iOS and Android offer to join the network when they see one.
import React, { useMemo } from 'react';
import { View } from 'react-native';
import { qrRuns } from '../wifiQr';
export { wifiQrPayload } from '../wifiQr';

export default function WifiQr({ value, size = 200 }: { value: string; size?: number }) {
  const { n, rows } = useMemo(() => qrRuns(value), [value]);
  const quiet = 4;                                        // the spec's white margin, in modules
  const cell = Math.max(2, Math.floor(size / (n + quiet * 2)));
  return (
    <View style={{ backgroundColor: '#fff', padding: cell * quiet, alignSelf: 'center', borderRadius: 8 }}
          accessibilityLabel="Wi-Fi QR code">
      {rows.map((runs, r) => (
        <View key={r} style={{ flexDirection: 'row', height: cell }}>
          {runs.map(([dark, len], i) => (
            <View key={i} style={{ width: cell * len, height: cell, backgroundColor: dark ? '#000' : '#fff' }} />
          ))}
        </View>
      ))}
    </View>
  );
}
