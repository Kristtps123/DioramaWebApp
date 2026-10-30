import { useState } from 'react';
import { Fingerprint, RotateCcw } from 'lucide-react';
import './TFTPreview.css';

const COLORS = {
  background: '#F5FAF7',
  card: '#FFFFFF',
  border: '#C4D9CD',
  track: '#D7E4DE',
  text: '#14231E',
  muted: '#788C84',
  green: '#10965F',
  amber: '#E68200',
  purple: '#7C3AED',
  red: '#DC323C',
  focus: '#06B6D4'
};

const MODES = ['Basic', 'Custom Color', 'Sound', 'Adaptive'];

function hsvToHex(hue, saturation, value = 100) {
  const h = hue / 60;
  const s = saturation / 100;
  const v = value / 100;
  const chroma = v * s;
  const x = chroma * (1 - Math.abs((h % 2) - 1));
  const match = v - chroma;
  const sector = Math.floor(h) % 6;
  const rgb = [
    [chroma, x, 0], [x, chroma, 0], [0, chroma, x],
    [0, x, chroma], [x, 0, chroma], [chroma, 0, x]
  ][sector].map((channel) => Math.round((channel + match) * 255));
  return `#${rgb.map((channel) => channel.toString(16).padStart(2, '0')).join('')}`;
}

function ScreenText({ x, y, children, size = 14, color = COLORS.text, anchor = 'start', weight = 400 }) {
  return (
    <text x={x} y={y} fill={color} fontSize={size} fontWeight={weight} textAnchor={anchor}>
      {children}
    </text>
  );
}

function ScreenButton({ x, y, width, height, children, active, accent = COLORS.amber, onClick, fontSize = 14, ariaLabel, disabled = false }) {
  return (
    <g
      className="tft-hit-target"
      role="button"
      tabIndex="0"
      aria-label={ariaLabel || children}
      aria-pressed={active}
      aria-disabled={disabled}
      opacity={disabled ? 0.5 : 1}
      onClick={disabled ? undefined : onClick}
      onKeyDown={(event) => {
        if (disabled) return;
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          onClick?.();
        }
      }}
    >
      <rect x={x} y={y} width={width} height={height} rx="7" fill={active ? accent : COLORS.card} stroke={active ? accent : COLORS.border} />
      <text x={x + width / 2} y={y + height / 2 + fontSize * 0.35} fill={active ? '#FFFFFF' : COLORS.text} fontSize={fontSize} fontWeight="600" textAnchor="middle">
        {children}
      </text>
    </g>
  );
}

function Slider({ x, y, width, value, accent = COLORS.amber }) {
  const thumbX = x + width * value / 100;
  return (
    <g>
      <rect x={x} y={y - 4} width={width} height="8" rx="4" fill={COLORS.track} />
      <rect x={x} y={y - 4} width={Math.max(0, thumbX - x)} height="8" rx="4" fill={accent} />
      <circle cx={thumbX} cy={y} r="11" fill={accent} stroke={COLORS.text} />
    </g>
  );
}

export default function TFTPreview() {
  const [screen, setScreen] = useState('welcome');
  const [page, setPage] = useState('Lights');
  const [mode, setMode] = useState('Basic');
  const [lightsOn, setLightsOn] = useState(true);
  const [brightness, setBrightness] = useState(75);
  const [colorSource, setColorSource] = useState('Scan Color');
  const [capturedColor, setCapturedColor] = useState('#D4B78C');
  const [scanStatus, setScanStatus] = useState('Captured color held');
  const [hue, setHue] = useState(36);
  const [saturation, setSaturation] = useState(46);
  const [sensitivity, setSensitivity] = useState(60);
  const [audioPlaying, setAudioPlaying] = useState(false);
  const [drainOn, setDrainOn] = useState(false);
  const [intakeOn, setIntakeOn] = useState(false);
  const [waterDetected, setWaterDetected] = useState(false);
  const [enrolled, setEnrolled] = useState(false);

  const selectedColor = hsvToHex(hue, saturation);
  const displayedColor = colorSource === 'Scan Color' ? capturedColor : selectedColor;

  const captureColor = () => {
    const nextColor = capturedColor === '#D4B78C' ? '#3B9DB3' : '#D4B78C';
    setCapturedColor(nextColor);
    setScanStatus('Color captured and applied');
  };

  const toggleWaterSensor = () => {
    const nextWaterDetected = !waterDetected;
    setWaterDetected(nextWaterDetected);
    if (nextWaterDetected) setIntakeOn(false);
  };

  const drawHeader = () => (
    <g>
      <rect width="480" height="36" fill={COLORS.card} />
      <line x1="0" y1="35" x2="480" y2="35" stroke={COLORS.border} />
      <ScreenButton x={4} y={3} width={76} height={30} active accent={COLORS.red} onClick={() => setScreen('welcome')} fontSize={13}>Exit</ScreenButton>
      <ScreenText x={92} y={22} size={13} color={COLORS.text}>Silvestre del Moro</ScreenText>
      <rect x="208" y="7" width="46" height="22" rx="5" fill={COLORS.green} />
      <ScreenText x={231} y={22} size={10} color="#FFFFFF" anchor="middle" weight={700}>TAB</ScreenText>
      {['Lights', 'Audio', 'Settings'].map((tab, index) => {
        const x = [260, 335, 405][index];
        const width = [75, 70, 75][index];
        const accent = [COLORS.amber, COLORS.green, COLORS.purple][index];
        return (
          <g key={tab} className="tft-hit-target" role="button" tabIndex="0" aria-label={`${tab} tab`} onClick={() => setPage(tab)}>
            <rect x={x} width={width} height="35" fill={page === tab ? accent : COLORS.card} />
            <ScreenText x={x + width / 2} y="22" size="13" color={page === tab ? '#FFFFFF' : COLORS.muted} anchor="middle" weight={600}>{tab}</ScreenText>
          </g>
        );
      })}
      <circle cx="474" cy="8" r="4" fill={COLORS.green} />
      <circle cx="474" cy="22" r="4" fill={COLORS.red} />
    </g>
  );

  const drawLights = () => {
    const soundMode = mode === 'Sound';
    const panelY = soundMode ? 136 : 204;
    return (
      <g>
        {MODES.map((item, index) => (
          <ScreenButton key={item} x={4 + index * 119} y={38} width={116} height={36} active={mode === item} onClick={() => setMode(item)} fontSize={item === 'Custom Color' ? 12 : 14}>
            {item}
          </ScreenButton>
        ))}

        <rect x="10" y="78" width="460" height="48" rx="8" fill={COLORS.card} stroke={lightsOn ? COLORS.amber : COLORS.border} />
        <circle cx="36" cy="102" r="14" fill={lightsOn ? COLORS.amber : COLORS.track} />
        <circle cx="36" cy="102" r="9" fill="none" stroke="#FFFFFF" strokeWidth="2" />
        <line x1="36" y1="87" x2="36" y2="96" stroke="#FFFFFF" strokeWidth="2" />
        <ScreenText x={60} y={99} size={20} weight={600}>{lightsOn ? 'Lights  ON' : 'Lights  OFF'}</ScreenText>
        <ScreenText x={60} y={119} size={13} color={COLORS.muted}>{mode === 'Custom Color' ? 'Custom Color' : mode}</ScreenText>
        <g className="tft-hit-target" role="button" tabIndex="0" aria-label="Toggle lights" aria-pressed={lightsOn} onClick={() => setLightsOn(!lightsOn)}>
          <rect x="410" y="91" width="44" height="22" rx="11" fill={lightsOn ? COLORS.amber : COLORS.track} />
          <circle cx={lightsOn ? 443 : 421} cy="102" r="8" fill="#FFFFFF" stroke={COLORS.border} />
        </g>

        {!soundMode && (
          <g>
            <ScreenText x={20} y={145} size={13} color={COLORS.muted}>{mode === 'Adaptive' ? 'Auto brightness' : 'Brightness'}</ScreenText>
            <ScreenText x={460} y={150} size={19} color={mode === 'Adaptive' ? COLORS.muted : COLORS.amber} anchor="end" weight={600}>{brightness}%</ScreenText>
            <Slider x={20} y={170} width={440} value={brightness} accent={mode === 'Adaptive' ? COLORS.muted : COLORS.amber} />
            <foreignObject x="20" y="156" width="440" height="28">
              <input className="tft-range" type="range" min="0" max="100" value={brightness} aria-label="TFT brightness" onChange={(event) => setBrightness(Number(event.target.value))} />
            </foreignObject>
          </g>
        )}

        {mode === 'Basic' && <ScreenText x={20} y={panelY + 18} size={13} color={COLORS.muted}>Steady white light. Adjust brightness above.</ScreenText>}

        {mode === 'Custom Color' && (
          <g>
            <ScreenText x="20" y="218" size="13" color={COLORS.muted}>Color source:</ScreenText>
            <ScreenButton x={190} y={206} width={130} height={24} active={colorSource === 'Scan Color'} onClick={() => setColorSource('Scan Color')} fontSize={11}>SCAN COLOR</ScreenButton>
            <ScreenButton x={330} y={206} width={140} height={24} active={colorSource === 'Hue Picker'} onClick={() => setColorSource('Hue Picker')} fontSize={11}>HUE PICKER</ScreenButton>
            <rect x="10" y="233" width="28" height="18" rx="4" fill={displayedColor} stroke={COLORS.text} />
            <ScreenText x="44" y="247" size="14" weight={600}>{displayedColor.toUpperCase()}</ScreenText>
            {colorSource === 'Scan Color' ? (
              <g>
                <ScreenText x="20" y="273" size="13" color={COLORS.muted}>{scanStatus}</ScreenText>
                <ScreenButton x={315} y={264} width={145} height={30} active={false} accent={COLORS.amber} onClick={captureColor} fontSize={11}>SCAN COLOR</ScreenButton>
              </g>
            ) : (
              <g>
                <ScreenText x="20" y="266" size="13" color={COLORS.muted}>Hue</ScreenText>
                <defs><linearGradient id="tftHueGradient">{Array.from({ length: 13 }, (_, index) => <stop key={index} offset={`${index / 12 * 100}%`} stopColor={`hsl(${index * 30} 100% 50%)`} />)}</linearGradient></defs>
                <rect x="20" y="268" width="440" height="8" rx="4" fill="url(#tftHueGradient)" stroke={COLORS.border} />
                <circle cx={20 + 440 * hue / 359} cy="272" r="8" fill={hsvToHex(hue, 100)} stroke={COLORS.text} />
                <foreignObject x="20" y="263" width="440" height="20">
                  <input className="tft-range tft-range-hue" type="range" min="0" max="359" value={hue} aria-label="TFT hue" onChange={(event) => setHue(Number(event.target.value))} />
                </foreignObject>
                <ScreenText x="20" y="292" size="13" color={COLORS.muted}>Saturation</ScreenText>
                <ScreenText x="460" y="292" size="15" color={COLORS.muted} anchor="end" weight={600}>{saturation}%</ScreenText>
                <rect x="20" y="294" width="440" height="8" rx="4" fill={`url(#tftSatGradient)`} stroke={COLORS.border} />
                <defs><linearGradient id="tftSatGradient"><stop offset="0%" stopColor="#FFFFFF" /><stop offset="100%" stopColor={hsvToHex(hue, 100)} /></linearGradient></defs>
                <circle cx={20 + 440 * saturation / 100} cy="298" r="8" fill={selectedColor} stroke={COLORS.text} />
                <foreignObject x="20" y="289" width="440" height="20">
                  <input className="tft-range tft-range-sat" type="range" min="0" max="100" value={saturation} aria-label="TFT saturation" onChange={(event) => setSaturation(Number(event.target.value))} />
                </foreignObject>
              </g>
            )}
          </g>
        )}

        {mode === 'Sound' && (
          <g>
            <ScreenText x="20" y="148" size="13" color={COLORS.muted}>Sound level</ScreenText>
            <rect x="20" y="154" width="440" height="20" fill={COLORS.track} />
            <rect x="20" y="154" width="190" height="20" fill={COLORS.green} />
            <ScreenText x="20" y="190" size="13" color={COLORS.muted}>Sensitivity</ScreenText>
            <ScreenText x="460" y="194" size="19" color={COLORS.amber} anchor="end" weight={600}>{sensitivity}%</ScreenText>
            <Slider x={20} y={216} width={440} value={sensitivity} />
            <foreignObject x="20" y="202" width="440" height="28">
              <input className="tft-range" type="range" min="0" max="100" value={sensitivity} aria-label="TFT sound sensitivity" onChange={(event) => setSensitivity(Number(event.target.value))} />
            </foreignObject>
          </g>
        )}

        {mode === 'Adaptive' && (
          <g>
            <ScreenText x="20" y="218" size="13" color={COLORS.muted}>Ambient light: 120.0 lx</ScreenText>
            <ScreenText x="20" y="244" size="13" color={COLORS.muted}>Adaptive brightness: {brightness}%</ScreenText>
          </g>
        )}
      </g>
    );
  };

  const drawAudio = () => (
    <g>
      <ScreenText x="240" y="64" size="22" color={COLORS.red} anchor="middle" weight={600}>Slave SD unavailable</ScreenText>
      <ScreenText x="240" y="106" size="14" color={COLORS.muted} anchor="middle">{audioPlaying ? 'Playing' : 'Stopped'}</ScreenText>
      <ScreenButton x={30} y={128} width={110} height={58} onClick={() => setAudioPlaying(false)} fontSize={16}>«</ScreenButton>
      <ScreenButton x={185} y={128} width={110} height={58} active={audioPlaying} accent={COLORS.green} onClick={() => setAudioPlaying(!audioPlaying)} fontSize={16}>{audioPlaying ? 'Pause' : 'Play'}</ScreenButton>
      <ScreenButton x={340} y={128} width={110} height={58} onClick={() => setAudioPlaying(false)} fontSize={16}>»</ScreenButton>
    </g>
  );

  const drawSettings = () => (
    <g>
      <ScreenText x="240" y="64" size="22" anchor="middle" weight={600}>Settings</ScreenText>
      <rect x="10" y="80" width="225" height="48" rx="8" fill={COLORS.card} stroke={drainOn ? COLORS.green : COLORS.border} />
      <ScreenText x="20" y="101" size="13" weight={600}>Drain Pump</ScreenText>
      <ScreenText x="20" y="119" size="13" color={drainOn ? COLORS.green : COLORS.muted}>{drainOn ? 'RUNNING' : 'STOPPED'}</ScreenText>
      <ScreenButton x={127} y={88} width={46} height={32} active={drainOn} accent={COLORS.green} onClick={() => setDrainOn(true)} fontSize={11}>ON</ScreenButton>
      <ScreenButton x={181} y={88} width={46} height={32} active={!drainOn} accent={COLORS.red} onClick={() => setDrainOn(false)} fontSize={11}>OFF</ScreenButton>
      <rect x="245" y="80" width="225" height="48" rx="8" fill={COLORS.card} stroke={intakeOn ? COLORS.green : COLORS.border} />
      <ScreenText x="255" y="101" size="13" weight={600}>Intake Pump</ScreenText>
      <ScreenText x="255" y="119" size="13" color={intakeOn ? COLORS.green : waterDetected ? COLORS.red : COLORS.muted}>{waterDetected ? 'WET - STOPPED' : intakeOn ? 'RUNNING' : 'STOPPED'}</ScreenText>
      <ScreenButton x={362} y={88} width={46} height={32} active={intakeOn} accent={COLORS.green} onClick={() => setIntakeOn(true)} fontSize={11} disabled={waterDetected}>ON</ScreenButton>
      <ScreenButton x={416} y={88} width={46} height={32} active={!intakeOn} accent={COLORS.red} onClick={() => setIntakeOn(false)} fontSize={11}>OFF</ScreenButton>
      <ScreenText x="20" y="140" size="13" color={COLORS.muted}>Fingerprint Management</ScreenText>
      <ScreenText x="240" y="158" size="13" color={COLORS.amber} anchor="middle">Preview Only</ScreenText>
      <ScreenButton x={40} y={172} width={180} height={40} active={false} accent={COLORS.green} onClick={() => setEnrolled(true)} fontSize={13}>Enroll New</ScreenButton>
      <ScreenText x="20" y="244" size="13" color={COLORS.muted}>Enrolled Users:</ScreenText>
      {enrolled ? (
        <g>
          <rect x="20" y="252" width="440" height="28" rx="6" fill={COLORS.card} stroke={COLORS.border} />
          <circle cx="35" cy="266" r="10" fill={COLORS.purple} />
          <ScreenText x="35" y="270" size="12" color="#FFFFFF" anchor="middle" weight={600}>1</ScreenText>
          <ScreenText x="55" y="271" size="13">Preview User</ScreenText>
          <ScreenButton x={400} y={258} width={50} height={16} active accent={COLORS.red} onClick={() => setEnrolled(false)} fontSize={9}>DEL</ScreenButton>
        </g>
      ) : <ScreenText x="240" y="270" size="13" color={COLORS.muted} anchor="middle">No users enrolled yet</ScreenText>}
    </g>
  );

  const reset = () => {
    setScreen('welcome');
    setPage('Lights');
    setMode('Basic');
    setLightsOn(true);
    setBrightness(75);
    setColorSource('Scan Color');
    setCapturedColor('#D4B78C');
    setScanStatus('Captured color held');
    setHue(36);
    setSaturation(46);
    setSensitivity(60);
    setAudioPlaying(false);
    setDrainOn(false);
    setIntakeOn(false);
    setWaterDetected(false);
    setEnrolled(false);
  };

  return (
    <main className="tft-preview-page">
      <header className="tft-preview-heading">
        <div>
          <p className="tft-preview-eyebrow">FIRMWARE DISPLAY</p>
          <h1>TFT Preview</h1>
        </div>
        <button type="button" className="tft-reset-button" onClick={reset} title="Reset preview">
          <RotateCcw size={16} /> Reset
        </button>
      </header>

      <section className="tft-device" aria-label="Simulated 480 by 320 TFT display">
        <div className="tft-device-label">
          <span>SILVESTRE DEL MORO PARK</span>
          <span>480 × 320</span>
        </div>
        <svg className="tft-screen" viewBox="0 0 480 320" role="img" aria-label={`${screen === 'welcome' ? 'Fingerprint welcome' : `${page} screen`} TFT preview`}>
          <rect width="480" height="320" fill={COLORS.background} />
          {screen === 'welcome' ? (
            <g>
              <rect width="480" height="4" fill={COLORS.green} />
              <ScreenText x="240" y="26" size="14" color={COLORS.muted} anchor="middle">Silvestre del Moro Park</ScreenText>
              <rect x="180" y="40" width="120" height="2" fill={COLORS.green} />
              <ScreenText x="240" y="90" size="28" anchor="middle" weight={600}>Hi! Welcome!</ScreenText>
              <ScreenText x="240" y="154" size="16" color={COLORS.muted} anchor="middle">Place your finger on the</ScreenText>
              <ScreenText x="240" y="184" size="16" color={COLORS.green} anchor="middle">biometric scanner</ScreenText>
              <ScreenText x="240" y="214" size="16" color={COLORS.muted} anchor="middle">to control your diorama.</ScreenText>
              <ScreenText x="240" y="258" size="14" color={COLORS.muted} anchor="middle">Wi-Fi: Diorama-Park</ScreenText>
              <ScreenText x="240" y="282" size="14" color={COLORS.amber} anchor="middle">Password: diorama123</ScreenText>
              <ScreenText x="240" y="306" size="14" color={COLORS.green} anchor="middle">Web app: http://192.168.4.1</ScreenText>
            </g>
          ) : (
            <g>
              {drawHeader()}
              {page === 'Lights' ? drawLights() : page === 'Audio' ? drawAudio() : drawSettings()}
            </g>
          )}
        </svg>
      </section>

      <footer className="tft-preview-controls">
        {screen === 'welcome' ? (
          <button type="button" className="tft-simulate-button" onClick={() => setScreen('open')}>
            <Fingerprint size={18} /> Simulate fingerprint unlock
          </button>
        ) : (
          <button type="button" className="tft-simulate-button secondary" onClick={() => setScreen('welcome')}>
            Show welcome screen
          </button>
        )}
        <button
          type="button"
          className="tft-simulate-button secondary"
          aria-pressed={waterDetected}
          onClick={toggleWaterSensor}
        >
          Simulate water sensor: {waterDetected ? 'Wet' : 'Clear'}
        </button>
        <span>Local preview only · no hardware commands</span>
      </footer>
    </main>
  );
}