// ESP32 Hardware Communication Service

let esp32Ip = localStorage.getItem('esp32_ip') || '192.168.100.138';

export const getEsp32Ip = () => esp32Ip;

export const setEsp32Ip = (ip) => {
  esp32Ip = ip.replace(/^https?:\/\//, '').replace(/\/.*$/, '');
  localStorage.setItem('esp32_ip', esp32Ip);
};

// Generic fetch sender with fallback handling
const sendEspCommand = async (endpoint, params = {}) => {
  const queryString = new URLSearchParams(params).toString();
  // If app is served directly from ESP32, use relative URL; otherwise connect to esp32Ip
  const isSelfHosted = window.location.hostname === esp32Ip || (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted 
    ? `${endpoint}${queryString ? '?' + queryString : ''}`
    : `http://${esp32Ip}${endpoint}${queryString ? '?' + queryString : ''}`;

  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);

    const response = await fetch(url, {
      method: 'GET',
      signal: controller.signal,
      mode: 'cors'
    });
    clearTimeout(timeoutId);

    if (!response.ok) {
      console.warn(`ESP32 responded with status ${response.status}`);
      return false;
    }
    return true;
  } catch (error) {
    console.warn(`ESP32 reachability check failed (${url}):`, error.message);
    return false;
  }
};

// Hardware command wrappers
export const sendLightControl = (isOn, brightness = 75) => {
  return sendEspCommand('/api/light', {
    state: isOn ? 'on' : 'off',
    brightness: brightness
  });
};

export const sendFountainControl = (isOn, leftStrength = 100, rightStrength = 75, pattern = 'Pulsing') => {
  return sendEspCommand('/api/fountain', {
    state: isOn ? 'on' : 'off',
    strength: leftStrength,
    auxStrength: rightStrength,
    pattern: pattern
  });
};

export const sendDrainagePumpControl = (isOn) => {
  return sendEspCommand('/api/drainage-pump', {
    state: isOn ? 'on' : 'off'
  });
};

export const sendIntakePumpControl = (isOn) => {
  return sendEspCommand('/api/intake-pump', {
    state: isOn ? 'on' : 'off'
  });
};

export const sendColorControl = (hexColor, target = 'left') => {
  // Convert hex #RRGGBB to R, G, B ints
  const cleanHex = hexColor.replace('#', '');
  const r = parseInt(cleanHex.substring(0, 2), 16) || 0;
  const g = parseInt(cleanHex.substring(2, 4), 16) || 0;
  const b = parseInt(cleanHex.substring(4, 6), 16) || 0;

  return sendEspCommand('/api/color', { r, g, b, target });
};

export const sendColorScanMode = (scanEnabled) => {
  return sendEspCommand('/api/color/source', {
    mode: scanEnabled ? 'scan' : 'picker'
  });
};

export const triggerColorScan = async () => {
  const isSelfHosted = window.location.hostname === esp32Ip || (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/color/scan' : `http://${esp32Ip}/api/color/scan`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);
    const response = await fetch(url, { method: 'GET', signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    return await response.json();
  } catch (err) {
    console.warn('triggerColorScan error:', err);
    return null;
  }
};

export const sendGateControl = (isOpen) => {
  return sendEspCommand('/api/gate', {
    state: isOpen ? 'open' : 'closed'
  });
};

export const sendModeControl = (mode) => {
  return sendEspCommand('/api/mode', {
    mode: mode
  });
};

export const sendSoundReactive = (isOn, intensity = 65) => {
  return sendEspCommand('/api/sound-reactive', {
    state: isOn ? 'on' : 'off',
    intensity
  });
};

export const sendForceSensorControl = (isOn) => {
  return sendEspCommand('/api/force-sensor', {
    state: isOn ? 'on' : 'off'
  });
};

export const sendControlSource = (source) => {
  return sendEspCommand('/api/control-source', { source });
};

export const sendMotorControl = (isOn, dir = 'fwd') => {
  return sendEspCommand('/api/motor', {
    state: isOn ? 'on' : 'off',
    dir
  });
};

/**
 * Poll the ESP32 for the full diorama state.
 * Returns the parsed JSON object, or null if unreachable.
 * The web app calls this every few seconds to stay in sync
 * with changes made directly on the TFT.
 */
export const getEsp32State = async () => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/state' : `http://${esp32Ip}/api/state`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2500);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    return await response.json();
  } catch {
    return null;
  }
};
export const getGateStatus = async () => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/gate/status' : `http://${esp32Ip}/api/gate/status`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    const data = await response.json();
    return typeof data.open === 'boolean' ? data.open : null;
  } catch {
    return null;
  }
};

// ── Audio / SD-card commands ──────────────────────────────────────────────────

/**
 * Fetch the list of audio files stored on the SD card.
 * Expects ESP32 to return: { "files": ["001.mp3", "002.mp3", ...] }
 */
export const getAudioFiles = async () => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/audio/files' : `http://${esp32Ip}/api/audio/files`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    const data = await response.json();
    return Array.isArray(data.files) ? data.files : null;
  } catch {
    return null;
  }
};

/**
 * Tell the ESP32 to play a specific track by filename.
 * Sends: GET /api/audio/play?file=<filename>
 */
export const sendAudioPlay = (filename) =>
  sendEspCommand('/api/audio/play', { file: filename });

/**
 * Tell the ESP32 to pause playback.
 */
export const sendAudioPause = () => sendEspCommand('/api/audio/pause');

/**
 * Ask the main ESP32 to select and play the adjacent slave playlist track.
 */
export const sendAudioNext = () => sendEspCommand('/api/audio/next');
export const sendAudioPrevious = () => sendEspCommand('/api/audio/previous');

/**
 * Tell the ESP32 to stop playback.
 */
export const sendAudioStop = () => sendEspCommand('/api/audio/stop');

// ── Sensor reads ──────────────────────────────────────────────────────────────

// Read sound sensor from ESP32: returns { detected: bool, level: 0-1023 }
export const getSoundLevel = async () => {
  const isSelfHosted = window.location.hostname === esp32Ip || (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/sound' : `http://${esp32Ip}/api/sound`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return { detected: false, level: 0 };
    const data = await response.json();
    // Sound response includes the current physical RGB output when available.
    return {
      detected: !!data.detected,
      level: Math.min(1023, Math.max(0, Number(data.level) || 0)),
      r: Math.min(255, Math.max(0, Number(data.r) || 0)),
      g: Math.min(255, Math.max(0, Number(data.g) || 0)),
      b: Math.min(255, Math.max(0, Number(data.b) || 0))
    };
  } catch {
    return { detected: false, level: 0 };
  }
};

// Read force sensor from ESP32: returns { active: bool, level: 0-1023 }
export const getForceLevel = async () => {
  const isSelfHosted = window.location.hostname === esp32Ip || (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/force' : `http://${esp32Ip}/api/force`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    const data = await response.json();
    return {
      active: !!data.active,
      level: Math.min(1023, Math.max(0, Number(data.level ?? data.force) || 0))
    };
  } catch {
    return null;
  }
};

// Fetch live telemetry for all 4 hardware sensors
export const getLiveSensors = async () => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/sensors' : `http://${esp32Ip}/api/sensors`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2500);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    return await response.json();
  } catch {
    return null;
  }
};

// ── Fingerprint Management ──────────────────────────────────────────────────

export const getFingerprintUsers = async () => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted ? '/api/fingerprint/users' : `http://${esp32Ip}/api/fingerprint/users`;
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return { users: [], sensorAvailable: false };
    return await response.json();
  } catch {
    return { users: [], sensorAvailable: false };
  }
};

export const enrollFingerprint = async ({ name, step = 0, id = null }) => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const params = new URLSearchParams();
  if (name) params.append('name', name);
  if (step) params.append('step', step);
  if (id != null) params.append('id', id);

  const url = isSelfHosted
    ? `/api/fingerprint/enroll?${params.toString()}`
    : `http://${esp32Ip}/api/fingerprint/enroll?${params.toString()}`;

  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 4000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) {
      const err = await response.json().catch(() => ({}));
      return { success: false, message: err.error || 'Enrollment request failed' };
    }
    return await response.json();
  } catch (error) {
    return { success: false, message: error.message || 'ESP32 unreachable' };
  }
};

export const deleteFingerprintUser = async (id) => {
  const isSelfHosted =
    window.location.hostname === esp32Ip ||
    (window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1');
  const url = isSelfHosted
    ? `/api/fingerprint/delete?id=${id}`
    : `http://${esp32Ip}/api/fingerprint/delete?id=${id}`;

  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);
    const response = await fetch(url, { signal: controller.signal, mode: 'cors' });
    clearTimeout(timeoutId);
    if (!response.ok) return { success: false, message: 'Delete failed' };
    return await response.json();
  } catch {
    return { success: false, message: 'ESP32 unreachable' };
  }
};
