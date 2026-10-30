import { useState } from 'react';
import {
  Lightbulb,
  Waves,
  Music,
  Play,
  Pause,
  ChevronRight,
  Lock,
  Unlock
} from 'lucide-react';
import DioramaCanvas from '../../components/DioramaCanvas';
import { getAudioFiles, sendAudioPlay, sendAudioPause } from '../../services/esp32Api';
import './Home.css';

export default function Home({ appState, setAppState, setActiveTab, showToast, onGateClose }) {
  const {
    lightsOn, brightness, lightingMode,
    reactionIntensity,
    fountainOn, fountainStrength, fountainColor,
    audioPlaying, audioTrack,
    autoDimming, circleColor, gateOpen
  } = appState;
  const lightingModeLabel = lightingMode === 'Colorful' ? 'Custom Color' : lightingMode;
  const isAdaptiveBrightness = lightingMode === 'Color Adaptive';
  const audioTitle = audioTrack
    .replace(/^.*[\\/]/, '')
    .replace(/\.[^/.]+$/, '')
    .replace(/[_-]/g, ' ');
  const isSoundReactive = lightingMode === 'Sound Reactive';

  const [isScanning, setIsScanning] = useState(false);

  // When the gate is open, tapping the biometric button closes it
  const handleBiometricScan = () => {
    if (isScanning) return;
    if (gateOpen) {
      // Close gate — delegate to App-level handler
      onGateClose();
      return;
    }
    // Gate is closed — shouldn't normally be reachable here since the lock screen
    // is shown at the App level, but handle gracefully just in case
    setIsScanning(true);
    showToast('🔍 Biometric sensor scanning fingerprint...');
    setTimeout(() => {
      setAppState((prev) => ({ ...prev, gateOpen: true }));
      setIsScanning(false);
      showToast('✅ Fingerprint verified! Gate opened.');
    }, 1800);
  };

  return (
    <div className="page-container">
      {/* Header Section */}
      <div className="page-header-text">
        <div className="section-tag">STA. ELENA, CAMARINES NORTE</div>
        <h1 className="page-main-title">Silvestre del Moro Park</h1>
        <p className="page-subtitle">
          Control and explore the miniature recreation of this iconic local landmark.
        </p>
      </div>

      {/* Interactive 3D Diorama Canvas */}
      <DioramaCanvas
        lightsOn={lightsOn}
        brightness={brightness}
        lightingMode={lightingMode}
        soundReactiveOn={appState.soundReactiveOn}
        fountainOn={fountainOn}
        fountainStrength={fountainStrength}
        fountainAuxStrength={appState.fountainAuxStrength}
        fountainForceSensorOn={appState.fountainForceSensorOn}
        fountainColor={fountainColor}
        fountainAuxColor={appState.fountainAuxColor}
        circleColor={circleColor}
        autoDimming={autoDimming}
        audioPlaying={audioPlaying}
        onGateClick={() => showToast('✨ Interactive Diorama: Front Gate Tapped!')}
      />

      {/* 3 Control Cards Grid */}
      <div className="controls-grid">
        {/* 1. LIGHTS CARD */}
        <div className={`control-card ${lightsOn ? 'active-card' : ''}`}>
          <div className="card-header">
            <div className="card-title-group" onClick={() => setActiveTab('lights')}>
              <div className="icon-badge green">
                <Lightbulb size={22} />
              </div>
              <div className="title-stack">
                <h3 className="card-title">Lights</h3>
                <span className="card-status-subtext">
                  {lightsOn ? `On - ${lightingModeLabel}` : 'Off'}
                </span>
              </div>
            </div>
            <label className="toggle-switch">
              <input
                type="checkbox"
                checked={lightsOn}
                onChange={(e) => setAppState((prev) => ({ ...prev, lightsOn: e.target.checked }))}
              />
              <span className="slider round"></span>
            </label>
          </div>

          {!isSoundReactive && (
            <div className="card-slider-group">
              <div className="slider-label-row">
                <span>{isAdaptiveBrightness ? 'Auto brightness' : 'Brightness'}</span>
                <span className="value-label">{brightness}%</span>
              </div>
              <input
                type="range"
                min="0"
                max="100"
                value={brightness}
                disabled={!lightsOn || isAdaptiveBrightness}
                className="custom-range-slider"
                aria-label={isAdaptiveBrightness ? 'Adaptive brightness (read only)' : 'Brightness'}
                onChange={(e) => {
                  if (!isAdaptiveBrightness) {
                    setAppState((prev) => ({ ...prev, brightness: Number(e.target.value) }));
                  }
                }}
              />
              {isAdaptiveBrightness && (
                <span className="card-status-subtext">Automatically controlled by the ambient light sensor</span>
              )}
            </div>
          )}
          {isSoundReactive && (
            <div className="card-slider-group">
              <div className="slider-label-row">
                <span>Mic sensitivity</span>
                <span className="value-label">{reactionIntensity}%</span>
              </div>
              <input
                type="range"
                min="0"
                max="100"
                value={reactionIntensity}
                className="custom-range-slider"
                aria-label="Mic sensitivity"
                onChange={(e) => setAppState((prev) => ({
                  ...prev,
                  reactionIntensity: Number(e.target.value)
                }))}
              />
              <span className="card-status-subtext">
                0% disables sound triggers; higher sensitivity reacts to quieter sounds.
              </span>
            </div>
          )}
        </div>

        {/* 2. FOUNTAIN CARD */}
        <div className={`control-card ${fountainOn ? 'active-card' : ''}`}>
          <div className="card-header">
            <div className="card-title-group" onClick={() => setActiveTab('fountain')}>
              <div className="icon-badge green">
                <Waves size={22} />
              </div>
              <div className="title-stack">
                <h3 className="card-title">Fountain</h3>
                <span className="card-status-subtext">
                  {fountainOn ? 'Flowing' : 'Off'}
                </span>
              </div>
            </div>
            <label className="toggle-switch">
              <input
                type="checkbox"
                checked={fountainOn}
                onChange={(e) => setAppState((prev) => ({ ...prev, fountainOn: e.target.checked }))}
              />
              <span className="slider round"></span>
            </label>
          </div>
        </div>

        {/* 3. AUDIO CARD */}
        <div className="control-card">
          <div className="card-header">
            <div className="card-title-group" onClick={() => setActiveTab('audio')}>
              <div className="icon-badge green">
                <Music size={22} />
              </div>
              <div className="title-stack">
                <h3 className="card-title">Audio</h3>
                <span className="card-status-subtext">{audioTitle}</span>
              </div>
            </div>
            <button
              className="audio-play-circle-btn"
              onClick={async () => {
                const next = !audioPlaying;
                if (next) {
                  let track = audioTrack !== 'No audio selected' ? audioTrack : '';
                  if (!track) {
                    const files = await getAudioFiles();
                    track = files[0] || '';
                  }
                  if (!track || !(await sendAudioPlay(track))) {
                    showToast('No playable tracks found on the slave SD card');
                    return;
                  }
                  setAppState((prev) => ({
                    ...prev,
                    audioPlaying: true,
                    audioTrack: track
                  }));
                  showToast(`Playing ${track}`);
                } else {
                  if (!(await sendAudioPause())) {
                    showToast('Could not reach the slave audio player');
                    return;
                  }
                  setAppState((prev) => ({ ...prev, audioPlaying: false }));
                  showToast('Audio Paused');
                }
              }}
            >
              {audioPlaying ? <Pause size={16} /> : <Play size={16} className="play-icon-offset" />}
            </button>
          </div>

        </div>

        {/* 4. BIOMETRIC GATE CARD */}
        <div className={`control-card biometric-gate-card ${gateOpen ? 'gate-open-card' : ''} ${isScanning ? 'gate-scanning-card' : ''}`}>
          <div className="card-header">
            <div className="card-title-group">
              <div className={`icon-badge ${gateOpen ? 'gate-icon-open' : 'gate-icon-closed'}`}>
                {gateOpen ? <Unlock size={20} /> : <Lock size={20} />}
              </div>
              <div className="title-stack">
                <h3 className="card-title">Gate Access</h3>
                <div className={`gate-inline-pill ${gateOpen ? 'open' : 'closed'}`}>
                  <span className="pill-dot"></span>
                  <span>{isScanning ? 'Scanning...' : gateOpen ? 'Gate: OPEN' : 'Gate: CLOSED'}</span>
                </div>
              </div>
            </div>
          </div>

          {/* Biometric Scanner Button */}
          <button
            id="biometric-scan-btn"
            className={`biometric-scan-btn ${isScanning ? 'scanning' : ''} ${gateOpen ? 'gate-is-open' : ''}`}
            onClick={handleBiometricScan}
            disabled={isScanning}
            aria-label="Biometric fingerprint scanner to open or close the gate"
          >
            <div className="biometric-inner">
              <span className="biometric-label">
                {isScanning
                  ? 'Verifying...'
                  : gateOpen
                  ? 'Tap to Close Gate'
                  : 'Tap to Open Gate'}
              </span>
            </div>
            {isScanning && <div className="biometric-scan-ring"></div>}
          </button>
        </div>
      </div>

      {/* Tip Banner */}
      <div
        className="tip-banner-card"
        onClick={() => {
          setActiveTab('lights');
          showToast('Navigating to Lights & Colors');
        }}
      >
        <span>Tip: open Lights &amp; Colors to change colors and modes</span>
        <ChevronRight size={20} className="tip-arrow" />
      </div>
    </div>
  );
}
