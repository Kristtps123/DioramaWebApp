import { useCallback, useEffect, useState } from 'react';
import { Cpu, Sun, Mic, Fingerprint, RefreshCw, Plus, Trash2, Search, Radio, Droplets } from 'lucide-react';
import { getLiveSensors, getFingerprintUsers, deleteFingerprintUser } from '../../services/esp32Api';
import './Settings.css';

const SENSOR_METADATA_KEYS = new Set([
  'name', 'label', 'type', 'id', 'source', 'connected', 'active',
  'available', 'enabled', 'status', 'state', 'detected'
]);

const formatSensorName = (key, sensor = {}) => {
  const explicitName = sensor.name || sensor.label || sensor.type;
  const name = String(explicitName || key)
    .replace(/([a-z0-9])([A-Z])/g, '$1 $2')
    .replace(/[_-]+/g, ' ')
    .trim();
  return name ? name.replace(/\b\w/g, (letter) => letter.toUpperCase()) : 'Sensor';
};

const getSensorStatus = (sensor) => {
  const status = String(sensor.status ?? sensor.state ?? '').toLowerCase();
  if (['offline', 'disconnected', 'error', 'unavailable', 'inactive'].includes(status)) return false;
  if (['online', 'connected', 'ready', 'active'].includes(status)) return true;

  for (const key of ['connected', 'active', 'available', 'enabled']) {
    if (typeof sensor[key] === 'boolean') return sensor[key];
  }

  // Legacy firmware reports mic telemetry without a separate availability flag.
  return true;
};

const formatSensorReadings = (sensor) => {
  const rgb = ['r', 'g', 'b'];
  if (rgb.every((key) => Number.isFinite(Number(sensor[key])))) {
    return `RGB ${rgb.map((key) => Math.round(Number(sensor[key]))).join(', ')}`;
  }

  const preferredKeys = ['value', 'reading', 'lux', 'temperature', 'humidity', 'pressure', 'level', 'percent', 'rms', 'peakToPeak'];
  const readings = preferredKeys
    .filter((key) => sensor[key] !== undefined && sensor[key] !== null && !SENSOR_METADATA_KEYS.has(key))
    .map((key) => {
      const value = sensor[key];
      const label = key === 'value' || key === 'reading'
        ? ''
        : key.replace(/([a-z0-9])([A-Z])/g, '$1 $2');
      const unit = key === 'lux' ? ' lx'
        : key === 'temperature' ? ' °C'
          : key === 'humidity' ? '%'
            : key === 'pressure' ? ' hPa'
              : key === 'percent' ? '%'
                : '';
      const formatted = typeof value === 'number' ? Number(value.toFixed(2)) : value;
      return `${label ? `${label}: ` : ''}${formatted}${unit}`;
    });

  if (typeof sensor.detected === 'boolean') readings.push(sensor.detected ? 'Detected' : 'No activity');
  return readings.length ? readings.join(' · ') : 'Active';
};

const parseSensorList = (payload) => {
  if (!payload || typeof payload !== 'object') return [];
  const reportedSensors = payload.sensors;
  const entries = Array.isArray(reportedSensors)
    ? reportedSensors.map((sensor, index) => [
      sensor && typeof sensor === 'object'
        ? sensor.id || sensor.name || `sensor-${index + 1}`
        : `sensor-${index + 1}`,
      sensor
    ])
    : reportedSensors && typeof reportedSensors === 'object'
      ? Object.entries(reportedSensors)
      : Object.entries(payload).filter(([, value]) => value && typeof value === 'object' && !Array.isArray(value));

  return entries
    .filter(([, sensor]) => sensor !== null && sensor !== undefined)
    .map(([key, reportedSensor]) => {
      const sensor = typeof reportedSensor === 'object'
        ? reportedSensor
        : { value: reportedSensor };
      return {
        id: String(sensor.id ?? key),
        name: formatSensorName(key, sensor),
        active: getSensorStatus(sensor),
        reading: formatSensorReadings(sensor),
        scanning: sensor.scanning === true,
        isColorSensor: /color.?sensor/i.test(`${key} ${sensor.name || ''} ${sensor.type || ''}`),
        kind: String(sensor.type ?? key).toLowerCase()
      };
    });
};

export default function SettingsPage({ appState, setAppState, showToast, onOpenRegister }) {
  const [sensors, setSensors] = useState([]);
  const [sensorLoadError, setSensorLoadError] = useState(false);
  const [isRefreshing, setIsRefreshing] = useState(false);
  
  // Fingerprint management state
  const [fingerprintUsers, setFingerprintUsers] = useState([]);
  const [isLoadingUsers, setIsLoadingUsers] = useState(false);
  const [sensorAvailable, setSensorAvailable] = useState(true);
  const [userSearch, setUserSearch] = useState('');

  const checkSensors = useCallback(async (manual = false) => {
    if (manual) setIsRefreshing(true);
    try {
      const data = await getLiveSensors();
      if (!data) {
        setSensorLoadError(true);
        if (manual) showToast('Could not read sensor status from the ESP32.');
        return;
      }
      setSensors(parseSensorList(data));
      setSensorLoadError(false);
      if (manual) showToast('✅ Sensors status refreshed from ESP32');
    } finally {
      if (manual) setIsRefreshing(false);
    }
  }, [showToast]);

  // Fetch fingerprint users
  const loadFingerprintUsers = useCallback(async () => {
    setIsLoadingUsers(true);
    try {
      const data = await getFingerprintUsers();
      if (data) {
        setFingerprintUsers(data.users || []);
        if (typeof data.sensorAvailable === 'boolean') {
          setSensorAvailable(data.sensorAvailable);
        }
      }
    } catch (error) {
      console.error('Failed to load fingerprint users:', error);
    }
    setIsLoadingUsers(false);
  }, []);

  // Delete user
  const handleDeleteUser = async (userId) => {
    if (!window.confirm(`Delete registered user ID #${userId}?`)) return;
    
    try {
      const res = await deleteFingerprintUser(userId);
      if (res && res.success) {
        showToast(`✅ ${res.message || 'User deleted'}`);
        loadFingerprintUsers();
      } else {
        showToast('❌ Failed to delete user');
      }
    } catch {
      showToast('❌ Failed to delete user');
    }
  };

  const normalizedSearch = userSearch.trim().toLowerCase();
  const filteredFingerprintUsers = fingerprintUsers.filter((user) =>
    String(user.name || '').toLowerCase().includes(normalizedSearch) ||
    String(user.id).includes(normalizedSearch)
  );

  useEffect(() => {
    const initialFetch = setTimeout(() => {
      checkSensors();
      loadFingerprintUsers();
    }, 0);
    const interval = setInterval(() => {
      checkSensors();
      loadFingerprintUsers();
    }, 3000);
    return () => {
      clearTimeout(initialFetch);
      clearInterval(interval);
    };
  }, [checkSensors, loadFingerprintUsers]);

  return (
    <div className="page-container">
      {/* Header */}
      <div className="page-header-text">
        <div className="section-breadcrumb settings-breadcrumb">PREFERENCES</div>
        <h1 className="page-main-title">Settings & Sensors</h1>
        <p className="page-subtitle">The little details behind your little world.</p>
      </div>

      {/* Sensor Status Card */}
      <div className="settings-single-grid">
        <div className="control-card">
          <div className="card-header">
            <div className="card-title-group">
              <div className="icon-badge sage">
                <Cpu size={22} />
              </div>
              <h3 className="card-title">Sensors</h3>
            </div>
            <button 
              type="button" 
              className={`btn-refresh-pill ${isRefreshing ? 'spinning' : ''}`}
              onClick={() => checkSensors(true)}
              title="Refresh status"
            >
              <RefreshCw size={14} />
              <span>Refresh</span>
            </button>
          </div>

          <div className="sensor-items-list" aria-live="polite">
            {sensorLoadError ? (
              <p className="sensor-list-message">Unable to reach the ESP32 sensor endpoint. Check the connection and refresh.</p>
            ) : sensors.length === 0 ? (
              <p className="sensor-list-message">No sensors are currently reported by the ESP32.</p>
            ) : sensors.map((sensor) => {
              const Icon = /finger|biometric/i.test(`${sensor.name} ${sensor.kind}`)
                ? Fingerprint
                : /ambient|light|lux/i.test(`${sensor.name} ${sensor.kind}`)
                  ? Sun
                  : /mic|sound|audio/i.test(`${sensor.name} ${sensor.kind}`)
                    ? Mic
                    : Radio;
              return (
                <div className="sensor-item-row" key={sensor.id}>
                  <div className="sensor-item-left">
                    <Icon size={18} className="sensor-item-icon blue" />
                    <span className="sensor-name">{sensor.name}</span>
                  </div>
                  <span className={`sensor-val ${sensor.active ? 'active' : 'inactive'}`}>
                    <span className={`dot ${sensor.active ? 'green-dot' : 'muted-dot'}`}></span>
                    {sensor.active
                      ? sensor.reading
                      : sensor.isColorSensor
                        ? sensor.scanning ? 'Scanning for color…' : 'Not scanning'
                        : 'Offline'}
                  </span>
                </div>
              );
            })}
          </div>

          <p className="card-description-subtext mt-3">
            Hardware status indicators showing sensor connectivity and readiness for diorama automation.
          </p>
        </div>
      </div>

      {/* Intake and drainage pump controls */}
      <div className="settings-single-grid mt-4">
        <div className="control-card">
          <div className="card-header">
            <div className="card-title-group">
              <div className="icon-badge sage">
                <Droplets size={22} />
              </div>
              <div>
                <h3 className="card-title">Water Intake Pump</h3>
                <p className="card-subtitle">Fills the tank · Slave GPIO 27</p>
              </div>
            </div>
            <button
              type="button"
              className={`drainage-pump-toggle ${appState.intakePumpOn ? 'is-on' : ''}`}
              aria-pressed={appState.intakePumpOn}
              onClick={() => setAppState({ intakePumpOn: !appState.intakePumpOn })}
            >
              {appState.intakePumpOn ? 'Turn OFF' : 'Turn ON'}
            </button>
          </div>
          <p className="card-description-subtext">
            Switches the intake pump driver input on the slave ESP32. Connect the pump through a suitable
            motor driver or relay module; do not power the pump from GPIO 27.
          </p>
          <div className={`drainage-pump-status ${appState.intakePumpOn ? 'is-on' : ''}`} role="status">
            Intake pump · Slave GPIO 27: {appState.intakePumpOn ? 'ON' : 'OFF'}
          </div>
        </div>

        <div className="control-card">
          <div className="card-header">
            <div className="card-title-group">
              <div className="icon-badge sage">
                <Droplets size={22} />
              </div>
              <div>
                <h3 className="card-title">Water Drainage Pump</h3>
                <p className="card-subtitle">Suction/drainage · Slave GPIO 33</p>
              </div>
            </div>
            <button
              type="button"
              className={`drainage-pump-toggle ${appState.drainagePumpOn ? 'is-on' : ''}`}
              aria-pressed={appState.drainagePumpOn}
              onClick={() => setAppState({ drainagePumpOn: !appState.drainagePumpOn })}
            >
              {appState.drainagePumpOn ? 'Turn OFF' : 'Turn ON'}
            </button>
          </div>
          <p className="card-description-subtext">
            Switches the drainage pump driver input on the slave ESP32. Use a suitable motor driver or relay;
            do not power the pump directly from GPIO 33.
          </p>
          <div className={`drainage-pump-status ${appState.drainagePumpOn ? 'is-on' : ''}`} role="status">
            Drainage pump · Slave GPIO 33: {appState.drainagePumpOn ? 'ON' : 'OFF'}
          </div>
        </div>
      </div>

      {/* Fingerprint Management Card */}
      <div className="settings-single-grid mt-4">
        <div className="control-card">
          <div className="card-header">
            <div className="card-title-group">
              <div className="icon-badge green">
                <Fingerprint size={22} />
              </div>
              <div>
                <h3 className="card-title">Fingerprint Management</h3>
                <p className="card-subtitle">
                  {sensorAvailable ? 'Physical Sensor Connected' : 'Sensor Ready'} • {fingerprintUsers.length} users registered
                </p>
              </div>
            </div>
            <button 
              type="button" 
              className="btn-enroll-pill"
              onClick={() => {
                if (onOpenRegister) onOpenRegister();
              }}
            >
              <Plus size={16} />
              <span>Register Fingerprint</span>
            </button>
          </div>

          {/* Hardware Pins Notice */}
          <div className="sensor-pins-info-box mt-3 mb-3">
            <Cpu size={16} className="text-green-600" />
            <span>
              <strong>Sensor Hardware Wiring:</strong> Sensor TX &rarr; ESP32 RX (GPIO 16) &bull; Sensor RX &rarr; ESP32 TX (GPIO 17)
            </span>
          </div>

          <label className="fingerprint-search">
            <Search size={17} aria-hidden="true" />
            <input
              type="search"
              value={userSearch}
              onChange={(event) => setUserSearch(event.target.value)}
              placeholder="Search users by name or slot"
              aria-label="Search fingerprint users by name or slot ID"
            />
          </label>

          {/* Users List */}
          <div className="fingerprint-users-list">
            {isLoadingUsers ? (
              <p className="users-loading">Loading users...</p>
            ) : fingerprintUsers.length === 0 ? (
              <p className="no-users-msg">No users enrolled yet. Register the first fingerprint from Settings.</p>
            ) : filteredFingerprintUsers.length === 0 ? (
              <p className="no-users-msg">No fingerprint users match “{userSearch.trim()}”.</p>
            ) : (
              filteredFingerprintUsers.map((user) => (
                <div key={user.id} className="user-item-row">
                  <div className="user-item-left">
                    <div className="user-id-badge">
                      <span>{user.id}</span>
                    </div>
                    <div className="user-info">
                      <span className="user-name">{user.name}</span>
                      <span className="user-id-text">Biometric Slot ID #{user.id}</span>
                    </div>
                  </div>
                  <button
                    className="btn-delete-user"
                    onClick={() => handleDeleteUser(user.id)}
                    title="Delete user"
                  >
                    <Trash2 size={16} />
                  </button>
                </div>
              ))
            )}
          </div>

          <p className="card-description-subtext mt-3">
            Manage enrolled fingerprints for biometric gate access. Changes sync between TFT and web app.
          </p>
        </div>
      </div>
    </div>
  );
}
