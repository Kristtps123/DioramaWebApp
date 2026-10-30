import React, { useState, useEffect, useRef } from 'react';
import { Fingerprint, Lock } from 'lucide-react';
import Navigation from './components/Navigation';
import Home from './Pages/Home/Home';
import LightAndColor from './Pages/Light&Color/Light&Color';
import Fountain from './Pages/Fountain/Fountain';
import Audio from './Pages/Audio/Audio';
import SettingsPage from './Pages/Settings/Settings';
import AboutPage from './Pages/About/About';
import FingerprintRegisterModal from './components/FingerprintRegisterModal';
import {
  sendLightControl,
  sendFountainControl,
  sendIntakePumpControl,
  sendDrainagePumpControl,
  sendColorControl,
  sendGateControl,
  sendModeControl,
  sendSoundReactive,
  sendForceSensorControl,
  getEsp32State
} from './services/esp32Api';
import './App.css';

function App() {
  const [activeTab, setActiveTab] = useState('home');
  const [isRegisterModalOpen, setIsRegisterModalOpen] = useState(false);

  // Gate-based access state
  // 'waiting'  → gate closed, listening for physical biometric sensor on TX/RX
  // 'scanning' → fingerprint scan/verification on hardware sensor
  // 'open'     → gate open, UI unlocked
  // 'goodbye'  → gate just closed, showing goodbye message briefly
  const [gateState, setGateState] = useState('waiting');
  const goodbyeTimer = useRef(null);
  const pollTimer    = useRef(null);

  useEffect(() => () => {
    if (goodbyeTimer.current) clearTimeout(goodbyeTimer.current);
  }, []);

  // Shared application state
  const [appState, setAppState] = useState({
    lightsOn: true,
    brightness: 75,
    lightingMode: 'Basic',
    soundReactiveOn: false,
    micSensitivity: 50,
    reactionIntensity: 60,
    autoDimming: false,
    ambientSensorOn: false,
    simulatedLight: 72,

    fountainOn: true,
    fountainStrength: 100,
    fountainAuxStrength: 100,
    fountainForceSensorOn: false,
    intakePumpOn: false,
    intakeWaterSensorAvailable: false,
    intakeWaterDetected: false,
    intakeWaterSensorAdc: 0,
    drainagePumpOn: false,

    fountainColor: '#77898D',
    fountainAuxColor: '#3B9DB3',

    audioPlaying: false,
    audioTrack: 'No audio selected',

    circleColor: '#D4B78C',
    colorSensorTarget: 'All',
    rfidGateEnabled: true,
    gateOpen: false,
    plazaRotationMode: 'Sensor'
  });

  // ── Gate helpers ──────────────────────────────────────────────────────────

  const openGate = () => {
    setGateState('open');
    setAppState((prev) => ({ ...prev, gateOpen: true }));
    sendGateControl(true);
  };

  const closeGate = () => {
    if (goodbyeTimer.current) clearTimeout(goodbyeTimer.current);
    setGateState('goodbye');
    setAppState((prev) => ({ ...prev, gateOpen: false }));
    sendGateControl(false);
    // Return to the fingerprint screen after a brief goodbye state.
    goodbyeTimer.current = setTimeout(() => {
      setGateState('waiting');
    }, 3000);
  };

  // ── Poll ESP32 full state to sync changes & detect physical biometric scans ──
  // Polls every 1s when waiting (responsive to physical biometric sensor) and every 3s when open
  useEffect(() => {
    const syncFromEsp32 = async () => {
      const s = await getEsp32State();
      if (!s) return; // ESP32 unreachable — keep current state
      if (gateState === 'goodbye') return;

      // ── Gate sync ──────────────────────────────────────────────────────
      if (s.gateOpen === true && gateState !== 'open') {
        // Physical biometric sensor or TFT opened the gate — unlock web app
        if (goodbyeTimer.current) clearTimeout(goodbyeTimer.current);
        setGateState('open');
        setAppState(prev => ({ ...prev, gateOpen: true }));
        return;
      }
      if (s.gateOpen === false && gateState === 'open') {
        // TFT or hardware closed the gate — show goodbye on web app
        if (goodbyeTimer.current) clearTimeout(goodbyeTimer.current);
        setGateState('goodbye');
        setAppState(prev => ({ ...prev, gateOpen: false }));
        goodbyeTimer.current = setTimeout(() => setGateState('waiting'), 3000);
        return;
      }

      // ── Full state sync (only when gate is open) ───────────────────────
      if (gateState !== 'open') return;

      setAppState(prev => {
        // Map ESP32 state fields → web app state fields
        // Only overwrite fields that actually changed to avoid re-render churn
        const next = { ...prev };
        let changed = false;

        const set = (key, val) => {
          if (val !== undefined && prev[key] !== val) {
            next[key] = val;
            changed = true;
          }
        };

        set('lightsOn',         s.lightsOn);
        set('brightness',       s.brightness);
        set('lightingMode',     s.lightingMode);
        set('soundReactiveOn',  s.soundReactive);
        set('reactionIntensity',s.soundIntensity);
        set('fountainOn',       s.fountainOn);
        set('fountainStrength', s.fountainStr);
        set('fountainAuxStrength', s.fountainAux);
        set('fountainForceSensorOn', s.fountainForceSensorOn);
        set('intakePumpOn',      s.intakePumpOn);
        set('intakeWaterSensorAvailable', s.intakeWaterSensorAvailable);
        set('intakeWaterDetected', s.intakeWaterDetected);
        set('intakeWaterSensorAdc', s.intakeWaterSensorAdc);
        set('drainagePumpOn',   s.drainagePumpOn);
        set('audioPlaying',     s.audioPlaying);
        set('audioTrack',       s.audioTrack || 'No audio selected');
        set('fountainColor',    s.fountainColor);
        set('fountainAuxColor', s.fountainAuxColor);
        set('circleColor',      s.circleColor);

        return changed ? next : prev;
      });
    };

    // When gate is waiting, poll fast (1000ms) for snappy response to physical fingerprint scan
    const intervalTime = gateState === 'open' ? 3000 : 1000;
    pollTimer.current = setInterval(syncFromEsp32, intervalTime);
    return () => {
      clearInterval(pollTimer.current);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [gateState]);

  // ── State update wrapper ──────────────────────────────────────────────────

  const updateAppState = (updater) => {
    setAppState((prev) => {
      const next = typeof updater === 'function' ? updater(prev) : { ...prev, ...updater };

      if (next.lightsOn !== prev.lightsOn || next.brightness !== prev.brightness) {
        sendLightControl(next.lightsOn, next.brightness);
      }
      if (
        next.fountainOn !== prev.fountainOn ||
        next.fountainStrength !== prev.fountainStrength ||
        next.fountainAuxStrength !== prev.fountainAuxStrength
      ) {
        sendFountainControl(next.fountainOn, next.fountainStrength, next.fountainAuxStrength);
      }
      if (next.drainagePumpOn !== prev.drainagePumpOn) {
        sendDrainagePumpControl(next.drainagePumpOn);
      }
      if (next.intakePumpOn !== prev.intakePumpOn) {
        sendIntakePumpControl(next.intakePumpOn);
      }
      // Mode first — Basic locks slave to white before any color traffic.
      if (next.lightingMode !== prev.lightingMode) {
        sendModeControl(next.lightingMode);
      }
      // Keep a captured Custom Color synchronized across every zone.
      if (next.lightingMode === 'Colorful') {
        const colorChanged =
          next.fountainColor !== prev.fountainColor ||
          next.fountainAuxColor !== prev.fountainAuxColor ||
          next.circleColor !== prev.circleColor;
        const allZonesMatch =
          next.fountainColor === next.fountainAuxColor &&
          next.fountainColor === next.circleColor;

        if (colorChanged && allZonesMatch) {
          sendColorControl(next.fountainColor, 'all');
        } else {
          if (next.fountainColor !== prev.fountainColor) {
            sendColorControl(next.fountainColor, 'left');
          }
          if (next.fountainAuxColor !== prev.fountainAuxColor) {
            sendColorControl(next.fountainAuxColor, 'right');
          }
          if (next.circleColor !== prev.circleColor) {
            sendColorControl(next.circleColor, 'center');
          }
        }
      }
      if (next.soundReactiveOn !== prev.soundReactiveOn || next.reactionIntensity !== prev.reactionIntensity) {
        sendSoundReactive(next.soundReactiveOn, next.reactionIntensity);
      }
      if (next.fountainForceSensorOn !== prev.fountainForceSensorOn) {
        sendForceSensorControl(next.fountainForceSensorOn);
      }

      return next;
    });
  };

  const showToast = () => {};

  // ── Gate Lock Overlay ─────────────────────────────────────────────────────

  const isGateOpen = gateState === 'open';
  const isScanning = gateState === 'scanning';
  const isGoodbye  = gateState === 'goodbye';

  if (!isGateOpen) {
    return (
      <div className="gate-lock-screen">
        <div className="gate-lock-card">
          {/* Park name */}
          <p className="gate-lock-park-name">Silvestre del Moro Park</p>

          {/* Icon */}
          <div className={`gate-lock-icon-ring ${isScanning ? 'scanning' : ''} ${isGoodbye ? 'goodbye' : 'listening'}`}>
            {isGoodbye
              ? <Lock size={40} strokeWidth={1.5} />
              : <Fingerprint size={42} strokeWidth={1.5} className={isScanning ? 'fp-scanning' : 'fp-listening'} />
            }
          </div>

          {/* Message */}
          <h1 className="gate-lock-title">
            {isGoodbye
              ? 'Goodbye!'
              : isScanning
              ? 'Scanning Fingerprint...'
              : 'Biometric Access'}
          </h1>

          <p className="gate-lock-subtitle">
            {isGoodbye
              ? 'Thank you for visiting.'
              : isScanning
              ? 'Verifying biometric data on sensor, please wait...'
              : (
                  'Place your registered finger on the physical biometric scanner to unlock'
              )}
          </p>

          {/* Live Sensor Status Pill */}
          {!isScanning && !isGoodbye && (
            <div className="gate-sensor-live-indicator">
              <span className="live-status-dot" />
              <span>Physical Sensor Active &bull; Ready</span>
            </div>
          )}

          {/* Scanning animation bar */}
          {isScanning && <div className="gate-lock-progress-bar"><div className="gate-lock-progress-fill" /></div>}

        </div>
      </div>
    );
  }

  // ── Main App (gate open) ──────────────────────────────────────────────────

  return (
    <div className="diorama-app-container">
      <Navigation
        activeTab={activeTab}
        setActiveTab={(tab) => {
          setActiveTab(tab);
          window.scrollTo({ top: 0, behavior: 'smooth' });
        }}
        onSettingsClick={() => {
          setActiveTab('settings');
          window.scrollTo({ top: 0, behavior: 'smooth' });
        }}
      />

      <main className="main-viewport">
        <div className="content-max-width">
          {activeTab === 'home' && (
            <Home
              appState={appState}
              setAppState={updateAppState}
              setActiveTab={setActiveTab}
              showToast={showToast}
              onGateClose={closeGate}
            />
          )}

          {activeTab === 'lights' && (
            <LightAndColor
              appState={appState}
              setAppState={updateAppState}
              showToast={showToast}
            />
          )}

          {activeTab === 'fountain' && (
            <Fountain
              appState={appState}
              setAppState={updateAppState}
              showToast={showToast}
            />
          )}

          {activeTab === 'audio' && (
            <Audio
              appState={appState}
              setAppState={updateAppState}
              showToast={showToast}
            />
          )}

          {activeTab === 'settings' && (
            <SettingsPage
              appState={appState}
              setAppState={updateAppState}
              showToast={showToast}
              onOpenRegister={() => setIsRegisterModalOpen(true)}
            />
          )}

          {activeTab === 'about' && (
            <AboutPage setActiveTab={setActiveTab} />
          )}
        </div>
      </main>

      {/* Global Fingerprint Registration Modal */}
      <FingerprintRegisterModal
        isOpen={isRegisterModalOpen}
        onClose={() => setIsRegisterModalOpen(false)}
        onSuccess={(name) => {
          showToast(`✅ Fingerprint registered for ${name}!`);
        }}
        showToast={showToast}
      />
    </div>
  );
}

export default App;
