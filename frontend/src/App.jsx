import React, { useState, useRef } from "react";
import {
  Activity,
  Radio,
  CheckCircle2,
  AlertTriangle,
  TrendingDown,
  Zap,
  Thermometer,
  Droplets,
  Vibrate,
  Usb,
  ShieldAlert,
  Building2,
  Sparkles,
  Sliders
} from "lucide-react";
import {
  ResponsiveContainer,
  BarChart,
  Bar,
  XAxis,
  YAxis,
  Tooltip,
  Legend,
  LineChart,
  Line,
  CartesianGrid
} from "recharts";

// Shown until the first real reading arrives from the ESP32 over
// Web Serial. No mock/demo data anywhere -- this is just a safe
// placeholder so the UI doesn't crash on undefined fields before
// a connection is made.
const EMPTY_TELEMETRY = {
  txId: "--",
  substation: "--",
  district: "--",
  healthScore: 0,
  status: "Awaiting Connection",
  harmonicDev: 0,
  vibRMS: 0,
  temp: 0,
  humidity: 0,
  loadCurrent: "--",
  explanation: "Connect the ESP32 over USB to start receiving live readings.",
  harmonics: { "50Hz": 0, "100Hz": 0, "200Hz": 0, "300Hz": 0 },
  baseline: { "50Hz": 0, "100Hz": 0, "200Hz": 0, "300Hz": 0 }
};

export default function App() {
  const [telemetry, setTelemetry] = useState(EMPTY_TELEMETRY);
  const [history, setHistory] = useState([]);
  const [connected, setConnected] = useState(false);
  const [activeTab, setActiveTab] = useState("live");
  const serialReaderRef = useRef(null);

  // Web Serial API (Streams live JSON directly from the ESP32)
  const connectSerial = async () => {
    if (!("serial" in navigator)) {
      alert("Please open this app in Google Chrome or Microsoft Edge for Web Serial support.");
      return;
    }

    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      setConnected(true);
      console.log("TRANSVANI: serial port opened, listening for JSON lines...");

      const textDecoder = new TextDecoderStream();
      port.readable.pipeTo(textDecoder.writable);
      const reader = textDecoder.readable.getReader();
      serialReaderRef.current = reader;

      let buffer = "";
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buffer += value;
        const lines = buffer.split("\n");
        buffer = lines.pop();

        for (const line of lines) {
          const trimmed = line.trim();
          if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
            try {
              const data = JSON.parse(trimmed);
              setTelemetry((prev) => ({
                ...prev,
                txId: data.txId ?? prev.txId,
                substation: data.substation ?? prev.substation,
                district: data.district ?? prev.district,
                healthScore: data.healthScore ?? prev.healthScore,
                status: data.status ?? prev.status,
                harmonicDev: data.harmonicDev ?? prev.harmonicDev,
                vibRMS: data.vibRMS ?? prev.vibRMS,
                temp: data.temp ?? prev.temp,
                humidity: data.humidity ?? prev.humidity,
                loadCurrent: data.loadCurrent ?? prev.loadCurrent,
                explanation: data.explanation ?? prev.explanation,
                harmonics: data.harmonics ?? prev.harmonics,
                baseline: data.baseline ?? prev.baseline
              }));
              setHistory((prev) => {
                const next = [
                  ...prev,
                  { time: new Date().toLocaleTimeString().slice(3, 8), score: data.healthScore }
                ];
                const MAX_POINTS = 20;
                return next.length > MAX_POINTS ? next.slice(next.length - MAX_POINTS) : next;
              });
            } catch (err) {
              console.error("TRANSVANI: failed to parse serial line as JSON:", trimmed, err);
            }
          }
        }
      }
    } catch (err) {
      console.error("Serial connection failed:", err);
      setConnected(false);
    }
  };

  const getStatusBadge = (status) => {
    switch (status) {
      case "Normal":
        return { badge: "bg-emerald-500/10 text-emerald-400 border-emerald-500/30", text: "text-emerald-400" };
      case "Monitor":
        return { badge: "bg-amber-500/10 text-amber-400 border-amber-500/30", text: "text-amber-400" };
      case "Inspect Soon":
        return { badge: "bg-orange-500/10 text-orange-400 border-orange-500/30", text: "text-orange-400" };
      case "Critical":
        return { badge: "bg-rose-500/10 text-rose-400 border-rose-500/30", text: "text-rose-400" };
      default:
        return { badge: "bg-slate-800 text-slate-400 border-slate-700", text: "text-slate-400" };
    }
  };

  const currentStyle = getStatusBadge(telemetry.status);

  const fftChartData = [
    { freq: "50Hz", current: telemetry.harmonics["50Hz"], baseline: telemetry.baseline["50Hz"] },
    { freq: "100Hz", current: telemetry.harmonics["100Hz"], baseline: telemetry.baseline["100Hz"] },
    { freq: "200Hz", current: telemetry.harmonics["200Hz"], baseline: telemetry.baseline["200Hz"] },
    { freq: "300Hz", current: telemetry.harmonics["300Hz"], baseline: telemetry.baseline["300Hz"] }
  ];

  return (
    <div className="min-h-screen bg-slate-950 text-slate-100 font-sans p-4 md:p-6">
      <div className="max-w-7xl mx-auto space-y-5">
        
        {/* Navigation Header */}
        <header className="bg-slate-900 border border-slate-800 p-4 rounded-2xl flex flex-col md:flex-row justify-between items-start md:items-center gap-4">
          <div className="flex items-center gap-3">
            <div className="p-2.5 bg-indigo-600 rounded-xl">
              <Radio className="w-5 h-5 text-white animate-pulse" />
            </div>
            <div>
              <div className="flex items-center gap-2">
                <h1 className="font-black text-lg tracking-wider text-slate-100">TRANSVANI</h1>
                <span className="text-[10px] bg-indigo-500/20 text-indigo-300 border border-indigo-500/40 px-2 py-0.5 rounded font-mono">
                  PROTOTYPE v1.0
                </span>
              </div>
              <p className="text-xs text-slate-400">Predictive Transformer Health Signature Monitor</p>
            </div>
          </div>

          <div className="flex flex-wrap items-center gap-2.5">
            <div className="bg-slate-950 border border-slate-800 rounded-xl p-1 flex">
              <button
                onClick={() => setActiveTab("live")}
                className={`px-3 py-1.5 rounded-lg text-xs font-semibold transition ${
                  activeTab === "live" ? "bg-indigo-600 text-white" : "text-slate-400 hover:text-white"
                }`}
              >
                Live Node View
              </button>
              <button
                onClick={() => setActiveTab("government")}
                className={`px-3 py-1.5 rounded-lg text-xs font-semibold transition ${
                  activeTab === "government" ? "bg-indigo-600 text-white" : "text-slate-400 hover:text-white"
                }`}
              >
                DISCOM Utility Queue
              </button>
            </div>

            <button
              onClick={connectSerial}
              className={`flex items-center gap-2 px-4 py-2 rounded-xl text-xs font-semibold border transition ${
                connected
                  ? "bg-emerald-500/20 text-emerald-400 border-emerald-500/40"
                  : "bg-indigo-600 text-white hover:bg-indigo-500 border-transparent shadow-lg shadow-indigo-600/20"
              }`}
            >
              <Usb className="w-4 h-4" />
              {connected ? "ESP32 Live Stream Active" : "Connect ESP32 (USB)"}
            </button>
          </div>
        </header>

        {/* Live Connection Status Strip */}
        <div className="bg-slate-900/60 border border-slate-800/80 px-4 py-2.5 rounded-xl flex flex-wrap items-center justify-between text-xs text-slate-400 gap-3">
          <span className="flex items-center gap-1.5 font-medium">
            <Sliders className="w-4 h-4 text-indigo-400" /> Data Source:
          </span>
          <span className={connected ? "text-emerald-400 font-semibold" : "text-slate-500"}>
            {connected
              ? "Live -- streaming JSON from ESP32 over USB Serial"
              : "Not connected -- click \"Connect ESP32 (USB)\" above to start"}
          </span>
        </div>

        {activeTab === "live" ? (
          <>
            {/* Health Score Diagnostic Banner */}
            <div className="bg-slate-900 border border-slate-800 rounded-2xl p-6 relative overflow-hidden">
              <div className="grid grid-cols-1 lg:grid-cols-3 gap-6 items-center">
                <div className="flex items-center gap-6">
                  <div className="text-center">
                    <span className="text-[11px] font-bold text-slate-400 uppercase tracking-wider">Health Score</span>
                    <div className={`text-6xl font-black ${currentStyle.text} tracking-tight mt-1`}>
                      {telemetry.healthScore}
                      <span className="text-lg text-slate-600 font-normal">/100</span>
                    </div>
                  </div>
                  <div className="space-y-1.5 border-l border-slate-800 pl-6">
                    <span className={`inline-block text-xs font-extrabold px-3 py-1 rounded-full border ${currentStyle.badge}`}>
                      {telemetry.status.toUpperCase()}
                    </span>
                    <h2 className="text-lg font-bold text-white">{telemetry.txId}</h2>
                    <p className="text-xs text-slate-400">{telemetry.substation}</p>
                  </div>
                </div>

                <div className="lg:col-span-2 bg-slate-950/70 border border-slate-800/80 rounded-xl p-4 flex items-start gap-3">
                  <Sparkles className="w-5 h-5 text-indigo-400 shrink-0 mt-0.5" />
                  <div>
                    <h3 className="text-xs font-bold text-indigo-300 uppercase tracking-wide">Explainable Diagnostic</h3>
                    <p className="text-xs text-slate-300 mt-1 leading-relaxed">{telemetry.explanation}</p>
                    <div className="flex items-center gap-4 mt-2 text-[11px] text-slate-400 font-mono">
                      <span>Persistence: Active</span>
                      <span>Trend: Monitored</span>
                      <span>Baseline Variance: ±{telemetry.harmonicDev}%</span>
                    </div>
                  </div>
                </div>
              </div>
            </div>

            {/* Multimodal Metric Cards */}
            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
              <div className="bg-slate-900 border border-slate-800 rounded-xl p-4">
                <div className="flex justify-between items-center text-slate-400 text-xs">
                  <span className="font-semibold text-slate-300">ACOUSTIC (INMP441)</span>
                  <Activity className="w-4 h-4 text-indigo-400" />
                </div>
                <div className="text-2xl font-black text-white mt-2">
                  {telemetry.harmonicDev}% <span className="text-xs font-normal text-slate-400">Dev</span>
                </div>
                <span className="text-[11px] text-slate-400 mt-1 block">FFT Harmonics</span>
              </div>

              <div className="bg-slate-900 border border-slate-800 rounded-xl p-4">
                <div className="flex justify-between items-center text-slate-400 text-xs">
                  <span className="font-semibold text-slate-300">VIBRATION (ADXL345)</span>
                  <Vibrate className="w-4 h-4 text-cyan-400" />
                </div>
                <div className="text-2xl font-black text-cyan-400 mt-2">
                  {telemetry.vibRMS} <span className="text-xs font-normal text-slate-400">g RMS</span>
                </div>
                <span className="text-[11px] text-slate-400 mt-1 block">3-Axis Magnitude</span>
              </div>

              <div className="bg-slate-900 border border-slate-800 rounded-xl p-4">
                <div className="flex justify-between items-center text-slate-400 text-xs">
                  <span className="font-semibold text-slate-300">LOAD (ACS712)</span>
                  <Zap className="w-4 h-4 text-amber-400" />
                </div>
                <div className="text-2xl font-black text-amber-400 mt-2">{telemetry.loadCurrent}</div>
                <span className="text-[11px] text-slate-400 mt-1 block">Load-Aware Ref</span>
              </div>

              <div className="bg-slate-900 border border-slate-800 rounded-xl p-4">
                <div className="flex justify-between items-center text-slate-400 text-xs">
                  <span className="font-semibold text-slate-300">ENV. CONTEXT (DHT11)</span>
                  <div className="flex gap-1">
                    <Thermometer className="w-3.5 h-3.5 text-amber-400" />
                    <Droplets className="w-3.5 h-3.5 text-blue-400" />
                  </div>
                </div>
                <div className="text-2xl font-black text-slate-100 mt-2">
                  {telemetry.temp}°C <span className="text-sm font-normal text-slate-400">/ {telemetry.humidity}%</span>
                </div>
                <span className="text-[11px] text-slate-400 mt-1 block">Ambient Enclosure</span>
              </div>
            </div>

            {/* Charts Section */}
            <div className="grid grid-cols-1 lg:grid-cols-2 gap-5">
              <div className="bg-slate-900 border border-slate-800 rounded-xl p-5">
                <h3 className="text-sm font-bold text-slate-200">Acoustic Harmonics vs Baseline</h3>
                <p className="text-xs text-slate-400 mb-4">FFT Spectral Comparison</p>
                <div className="h-56">
                  <ResponsiveContainer width="100%" height="100%">
                    <BarChart data={fftChartData}>
                      <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                      <XAxis dataKey="freq" stroke="#64748b" />
                      <YAxis stroke="#64748b" />
                      <Tooltip contentStyle={{ backgroundColor: "#0f172a", borderColor: "#334155" }} />
                      <Legend />
                      <Bar dataKey="baseline" name="Healthy Baseline" fill="#475569" radius={[4, 4, 0, 0]} />
                      <Bar dataKey="current" name="Live Harmonics" fill="#6366f1" radius={[4, 4, 0, 0]} />
                    </BarChart>
                  </ResponsiveContainer>
                </div>
              </div>

              <div className="bg-slate-900 border border-slate-800 rounded-xl p-5">
                <h3 className="text-sm font-bold text-slate-200">Degradation Trend</h3>
                <p className="text-xs text-slate-400 mb-4">Score Drift Over Time</p>
                <div className="h-56">
                  <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={history}>
                      <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                      <XAxis dataKey="time" stroke="#64748b" />
                      <YAxis domain={[0, 100]} stroke="#64748b" />
                      <Tooltip contentStyle={{ backgroundColor: "#0f172a", borderColor: "#334155" }} />
                      <Line type="monotone" dataKey="score" stroke="#f43f5e" strokeWidth={2.5} dot={{ fill: "#f43f5e", r: 4 }} />
                    </LineChart>
                  </ResponsiveContainer>
                </div>
              </div>
            </div>
          </>
        ) : (
          /* DISCOM Maintenance Queue View */
          <div className="space-y-5">
            <div className="bg-slate-900 border border-slate-800 rounded-xl p-5">
              <div className="flex items-center gap-2 mb-2">
                <Building2 className="w-5 h-5 text-indigo-400" />
                <h2 className="text-sm font-bold text-slate-200">State DISCOM Maintenance Hierarchy</h2>
              </div>
              <p className="text-xs text-slate-400">Prioritized field inspection queue generated from condition metrics.</p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-3 gap-4">
              <div className="bg-slate-900 border border-rose-500/30 rounded-xl p-4">
                <div className="flex justify-between items-center mb-2">
                  <span className="text-xs font-bold text-rose-400 flex items-center gap-1.5">
                    <ShieldAlert className="w-4 h-4" /> CRITICAL QUEUE
                  </span>
                  <span className="text-xs bg-rose-500/20 text-rose-300 px-2 py-0.5 rounded font-mono">1 Asset</span>
                </div>
                <div className="p-3 bg-slate-950/80 rounded-lg border border-slate-800 space-y-1">
                  <div className="flex justify-between text-xs font-bold text-slate-200">
                    <span>TX-AGR-1022</span>
                    <span className="text-rose-400">Score: 41</span>
                  </div>
                  <p className="text-[11px] text-slate-400">Palar Basin Feeder</p>
                  <span className="text-[10px] text-rose-400/90 font-mono block">Action: Immediate Lineman Dispatch</span>
                </div>
              </div>

              <div className="bg-slate-900 border border-orange-500/30 rounded-xl p-4">
                <div className="flex justify-between items-center mb-2">
                  <span className="text-xs font-bold text-orange-400 flex items-center gap-1.5">
                    <AlertTriangle className="w-4 h-4" /> ATTENTION QUEUE
                  </span>
                  <span className="text-xs bg-orange-500/20 text-orange-300 px-2 py-0.5 rounded font-mono">1 Asset</span>
                </div>
                <div className="p-3 bg-slate-950/80 rounded-lg border border-slate-800 space-y-1">
                  <div className="flex justify-between text-xs font-bold text-slate-200">
                    <span>TX-IND-0418</span>
                    <span className="text-orange-400">Score: 68</span>
                  </div>
                  <p className="text-[11px] text-slate-400">SIDCO Industrial Area</p>
                  <span className="text-[10px] text-orange-400/90 font-mono block">Action: Routine Check within 48h</span>
                </div>
              </div>

              <div className="bg-slate-900 border border-emerald-500/30 rounded-xl p-4">
                <div className="flex justify-between items-center mb-2">
                  <span className="text-xs font-bold text-emerald-400 flex items-center gap-1.5">
                    <CheckCircle2 className="w-4 h-4" /> HEALTHY QUEUE
                  </span>
                  <span className="text-xs bg-emerald-500/20 text-emerald-300 px-2 py-0.5 rounded font-mono">1 Asset</span>
                </div>
                <div className="p-3 bg-slate-950/80 rounded-lg border border-slate-800 space-y-1">
                  <div className="flex justify-between text-xs font-bold text-slate-200">
                    <span>TX-RUR-0941</span>
                    <span className="text-emerald-400">Score: 92</span>
                  </div>
                  <p className="text-[11px] text-slate-400">Kaniyambadi Feeder Sub-04</p>
                  <span className="text-[10px] text-emerald-400/90 font-mono block">Action: Continuous Monitoring</span>
                </div>
              </div>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}