export const mockData = {
  normal: {
    txId: "TX-RUR-0941",
    substation: "Vellore 110/11kV Sub-02",
    district: "Vellore",
    healthScore: 92,
    status: "Normal",
    harmonicDev: 11.8,
    vibRMS: 0.32,
    temp: 29.4,
    humidity: 61.0,
    loadCurrent: "4.1 A",
    explanation: "Acoustic harmonics align with healthy baseline. Vibration and thermal context stable.",
    harmonics: { "50Hz": 82, "100Hz": 95, "200Hz": 29, "300Hz": 14 },
    baseline: { "50Hz": 80, "100Hz": 94, "200Hz": 28, "300Hz": 14 }
  },
  monitor: {
    txId: "TX-RUR-0941",
    substation: "Vellore 110/11kV Sub-02",
    district: "Vellore",
    healthScore: 78,
    status: "Monitor",
    harmonicDev: 24.5,
    vibRMS: 0.45,
    temp: 30.1,
    humidity: 63.0,
    loadCurrent: "4.4 A",
    explanation: "Isolated acoustic spike detected. Persistence filter active; no fault declared.",
    harmonics: { "50Hz": 85, "100Hz": 98, "200Hz": 48, "300Hz": 28 },
    baseline: { "50Hz": 80, "100Hz": 94, "200Hz": 28, "300Hz": 14 }
  },
  critical: {
    txId: "TX-RUR-0941",
    substation: "Vellore 110/11kV Sub-02",
    district: "Vellore",
    healthScore: 41,
    status: "Critical",
    harmonicDev: 68.2,
    vibRMS: 1.15,
    temp: 33.8,
    humidity: 58.0,
    loadCurrent: "4.9 A",
    explanation: "Persistent 100Hz+ harmonics and structural vibration escalation. Immediate dispatch required.",
    harmonics: { "50Hz": 88, "100Hz": 115, "200Hz": 72, "300Hz": 54 },
    baseline: { "50Hz": 80, "100Hz": 94, "200Hz": 28, "300Hz": 14 }
  }
};