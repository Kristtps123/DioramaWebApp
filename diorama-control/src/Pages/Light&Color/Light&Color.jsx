import { useEffect, useRef, useState } from 'react';
import {
  Lightbulb,
  Mic,
  Palette,
  Sun,
  Check
} from 'lucide-react';
import DioramaCanvas from '../../components/DioramaCanvas';
import ColorPicker from '../../components/ColorPicker';
import { getLiveSensors, sendColorScanMode, triggerColorScan } from '../../services/esp32Api';
import './Light&Color.css';

const LIGHTING_MODES = [
  {
    id: 'Basic',
    name: 'Basic',
    desc: 'Steady white RGB lighting across the whole diorama',
    icon: Lightbulb
  },
  {
    id: 'Colorful',
    name: 'Custom Color',
    desc: 'Capture a color with the sensor or choose one with the hue picker',
    icon: Palette
  },
  {
    id: 'Sound Reactive',
    name: 'Sound Reactive',
    desc: 'Lights & inner circle react to sound sensor',
    icon: Mic
  },
  {
    id: 'Color Adaptive',
    name: 'Color Adaptive',
    desc: 'Follows ambient light sensor automatically',
    icon: Sun
  }
];

const rgbToHex = (r, g, b) => {
  const c = value => Math.max(0, Math.min(255, value)).toString(16).padStart(2, '0');
  return `#${c(r)}${c(g)}${c(b)}`;
};

const hexToRgb = (hex) => {
  const value = Number.parseInt((hex || '#D4B78C').replace('#', ''), 16);
  return { r: (value >> 16) & 255, g: (value >> 8) & 255, b: value & 255 };
};

export default function LightAndColor({ appState, setAppState, showToast }) {
  const {
    lightsOn, brightness, lightingMode, soundReactiveOn,
    reactionIntensity, circleColor, fountainColor, autoDimming
  } = appState;
  const isColorAdaptive = lightingMode === 'Color Adaptive';

  const [currentColor, setCurrentColor] = useState(circleColor || '#D4B78C');
  const [ambientTelemetry, setAmbientTelemetry] = useState({ connected: false, lux: null, brightness: null });
  const [colorTelemetry, setColorTelemetry] = useState({ connected: false, scanning: false, r: 255, g: 255, b: 255 });
  const [colorScanMode, setColorScanMode] = useState(true);
  const [sourceChanging, setSourceChanging] = useState(false);
  const colorScanModeRef = useRef(colorScanMode);

  useEffect(() => {
    let active = true;
    const refreshSensors = async () => {
      const data = await getLiveSensors();
      if (active && data) {
        if (data.ambientLight) setAmbientTelemetry(data.ambientLight);
        if (data.colorSensor) {
          setColorTelemetry(data.colorSensor);
          const scanMode = data.colorSensor.scanMode
            ? data.colorSensor.scanMode === 'scan'
            : colorScanModeRef.current;
          colorScanModeRef.current = scanMode;
          setColorScanMode(scanMode);
        }
      }
    };
    refreshSensors();
    const interval = setInterval(refreshSensors, 500);
    return () => {
      active = false;
      clearInterval(interval);
    };
  }, []);

  const activeModeName = LIGHTING_MODES.find((mode) => mode.id === lightingMode)?.name || lightingMode;

  const handleColorScan = async () => {
    if (sourceChanging) return;
    setSourceChanging(true);
    try {
      if (!colorScanModeRef.current) {
        const updated = await sendColorScanMode(true);
        if (!updated) {
          showToast('Could not change the color source. Check the ESP32 connection.');
          return;
        }
        colorScanModeRef.current = true;
        setColorScanMode(true);
      }

      const result = await triggerColorScan();
      if (!result || result.success !== true) {
        showToast('Could not scan the color. Check that the sensor is connected and try again.');
        return;
      }

      const scannedColor = rgbToHex(result.r, result.g, result.b);
      setCurrentColor(scannedColor);
      setAppState((prev) => ({
        ...prev,
        circleColor: scannedColor,
        fountainColor: scannedColor,
        fountainAuxColor: scannedColor
      }));
      showToast(`Color captured: ${scannedColor.toUpperCase()}`);
    } finally {
      setSourceChanging(false);
    }
  };

  const handleColorSourceChange = async (scanEnabled) => {
    if (sourceChanging || scanEnabled === colorScanModeRef.current) return;
    setSourceChanging(true);
    try {
      const updated = await sendColorScanMode(scanEnabled);
      if (!updated) {
        showToast('Could not change the color source. Check the ESP32 connection.');
        return;
      }

      colorScanModeRef.current = scanEnabled;
      setColorScanMode(scanEnabled);
    } finally {
      setSourceChanging(false);
    }
  };

  const handlePickerChange = ({ r, g, b }) => {
    const selectedColor = rgbToHex(r, g, b);
    setCurrentColor(selectedColor);
    setAppState((prev) => ({
      ...prev,
      circleColor: selectedColor,
      fountainColor: selectedColor,
      fountainAuxColor: selectedColor
    }));
  };

  // Mode Selection Handler
  const handleSelectMode = (modeId) => {
    if (modeId === 'Basic') {
      setAppState((prev) => ({
        ...prev,
        lightingMode: 'Basic',
        soundReactiveOn: false,
        ambientSensorOn: false,
        circleColor: '#FFFFFF',
        fountainColor: '#FFFFFF',
        fountainAuxColor: '#FFFFFF'
      }));
    } else if (modeId === 'Colorful') {
      setAppState((prev) => ({ ...prev, lightingMode: 'Colorful', soundReactiveOn: false, ambientSensorOn: false }));
    } else if (modeId === 'Sound Reactive') {
      setAppState((prev) => ({ ...prev, lightingMode: 'Sound Reactive', soundReactiveOn: true, ambientSensorOn: false }));
    } else if (modeId === 'Color Adaptive') {
      setAppState((prev) => ({ ...prev, lightingMode: 'Color Adaptive', soundReactiveOn: false, ambientSensorOn: true }));
    }
  };

  return (
    <div className="page-container compact-for-mobile">
      {/* Header */}
      <div className="page-header-text">
        <div className="section-breadcrumb">CONTROLS / 01</div>
        <h1 className="page-main-title">Lights & Colors</h1>
        <p className="page-subtitle">Set the mood and color of your diorama.</p>
      </div>

      {/* 3D Diorama Preview */}
      <DioramaCanvas
        lightsOn={lightsOn}
        brightness={brightness}
        lightingMode={lightingMode}
        soundReactiveOn={soundReactiveOn}
        fountainOn={appState.fountainOn}
        fountainStrength={appState.fountainStrength}
        fountainAuxStrength={appState.fountainAuxStrength}
        fountainForceSensorOn={appState.fountainForceSensorOn}
        fountainPattern={appState.fountainPattern}
        fountainColor={fountainColor}
        fountainAuxColor={appState.fountainAuxColor}
        circleColor={circleColor}
        autoDimming={autoDimming}
        audioPlaying={appState.audioPlaying}
        onGateClick={() => showToast('✨ Diorama Gate Tapped!')}
      />

      {/* SECTION: 4-GRID LIGHTING MODES */}
      <div className="section-block">
        <div className="section-header-row">
          <h2 className="section-block-title">Lighting Modes</h2>
          <span className="section-sub-badge">{activeModeName}</span>
        </div>

        <div className="modes-grid">
          {LIGHTING_MODES.map((mode) => {
            const IconComponent = mode.icon;
            const isSelected = lightingMode === mode.id;
            return (
              <div
                key={mode.id}
                className={`mode-card ${isSelected ? 'selected' : ''}`}
                onClick={() => handleSelectMode(mode.id)}
                role="button"
                tabIndex={0}
              >
                <div className="mode-card-header">
                  <div className={`icon-badge ${isSelected ? 'amber-active' : 'amber'}`}>
                    <IconComponent size={20} />
                  </div>
                  {isSelected && (
                    <div className="mode-active-pill">
                      <Check size={14} />
                      <span>Active</span>
                    </div>
                  )}
                </div>
                <div className="mode-card-body">
                  <h4 className="mode-name">{mode.name}</h4>
                  <p className="mode-desc">{mode.desc}</p>
                </div>
              </div>
            );
          })}
        </div>

        {/* Sound Reactive Sub-Control (when Sound Reactive is active) */}
        {lightingMode === 'Sound Reactive' && (
          <div className="control-card mt-2">
            <div className="card-header">
              <div className="card-title-group">
                <div className="icon-badge amber">
                  <Mic size={20} />
                </div>
                <div className="title-stack">
                  <h3 className="card-title">Sound-reactive lighting</h3>
                  <span className="card-status-subtext">Active — RGB lighting follows the sound sensor</span>
                </div>
              </div>
            </div>
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
                className="custom-range-slider amber-range"
                aria-label="Mic sensitivity"
                onChange={(event) => setAppState((prev) => ({
                  ...prev,
                  reactionIntensity: Number(event.target.value)
                }))}
              />
              <span className="card-status-subtext">
                0% disables sound triggers; higher sensitivity reacts to quieter sounds.
              </span>
            </div>
          </div>
        )}

        {/* Colorful Mode: choose automatic scanning or manual color picking */}
        {lightingMode === 'Colorful' && (
          <div className="control-card mt-2 color-collapsible-card">
            <div className="card-header">
              <div className="card-title-group">
                <div className="icon-badge amber">
                  <Palette size={20} />
                </div>
                <div className="title-stack">
                  <div className="title-with-swatch">
                    <h3 className="card-title">Color Control</h3>
                    <span
                      className="color-mini-swatch"
                      style={{ backgroundColor: currentColor }}
                      title={currentColor}
                    />
                    <span className="hex-mini-tag">{(currentColor || '').toUpperCase()}</span>
                  </div>
                  <span className="card-status-subtext">
                    Capture a color once with the sensor or use the hue picker.
                  </span>
                </div>
              </div>
            </div>

            <div className="color-collapsible-body">
              <div className="color-source-control" role="group" aria-label="Custom color source">
                <button
                  type="button"
                  className={`color-source-option ${colorScanMode ? 'active' : ''}`}
                  aria-pressed={colorScanMode}
                  disabled={sourceChanging}
                  onClick={handleColorScan}
                >
                  Scan Color
                </button>
                <button
                  type="button"
                  className={`color-source-option ${!colorScanMode ? 'active' : ''}`}
                  aria-pressed={!colorScanMode}
                  disabled={sourceChanging}
                  onClick={() => handleColorSourceChange(false)}
                >
                  Hue Picker
                </button>
              </div>

              <div className="detected-color-row">
                <span className="detected-color-status">
                  <span
                  className="color-mini-swatch"
                  style={{ backgroundColor: currentColor }}
                  />
                  {colorScanMode
                  ? colorTelemetry.connected
                    ? `Sensor ready — held color ${currentColor.toUpperCase()}`
                    : 'Waiting for color sensor reading…'
                  : `Picker color ${currentColor.toUpperCase()} — applied to all zones`}
                </span>
                <span className="hex-display">{(currentColor || '').toUpperCase()}</span>
              </div>
              {colorScanMode
                ? <p className="card-description-subtext mt-1">
                    Press Scan Color to capture the object. The lights keep that color until you scan again.
                  </p>
                : <ColorPicker rgb={hexToRgb(currentColor)} onChange={handlePickerChange} />}
            </div>
          </div>
        )}

        {/* Color Adaptive: Ambient Sensor Status Banner */}
        {isColorAdaptive && (
          <div className="control-card mt-2 ambient-status-card">
            <div className="card-header">
              <div className="card-title-group">
                <div className="icon-badge amber">
                  <Sun size={20} />
                </div>
                <div className="title-stack">
                  <h3 className="card-title">Ambient Light Sensor</h3>
                  <span className="card-status-subtext" style={{ color: ambientTelemetry.connected ? '#4ade80' : '#f87171' }}>
                    {ambientTelemetry.connected
                      ? `● ${Number(ambientTelemetry.lux).toFixed(1)} lx — output ${ambientTelemetry.brightness}%`
                      : '● Ambient light sensor offline — adaptive dimming paused'}
                  </span>
                </div>
              </div>
              <span className="sensor-scanning-pill" style={{
                background: ambientTelemetry.connected ? 'rgba(74,222,128,0.15)' : 'rgba(248,113,113,0.15)',
                color: ambientTelemetry.connected ? '#4ade80' : '#f87171',
                border: `1px solid ${ambientTelemetry.connected ? 'rgba(74,222,128,0.3)' : 'rgba(248,113,113,0.3)'}`
              }}>
                <span className="scanning-dot" style={{ background: ambientTelemetry.connected ? '#4ade80' : '#f87171' }}></span>
                {ambientTelemetry.connected ? 'AMBIENT SENSOR ON' : 'SENSOR OFF'}
              </span>
            </div>
            <p className="card-description-subtext mt-1">
              {ambientTelemetry.connected
                ? 'Adaptive brightness follows lux readings from the VEML7700 on I2C.'
                : 'Waiting for ambient-light readings from the VEML7700.'}
            </p>
          </div>
        )}
      </div>


    </div>
  );
}
