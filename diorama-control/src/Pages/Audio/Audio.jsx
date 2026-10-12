import { useEffect, useState, useCallback } from 'react';
import { Music, Play, Pause, RefreshCw, AlertCircle } from 'lucide-react';
import { getAudioFiles, sendAudioPlay, sendAudioPause } from '../../services/esp32Api';
import './Audio.css';

export default function Audio({ appState, setAppState, showToast }) {
  const { audioPlaying, audioTrack } = appState;

  const [tracks, setTracks]     = useState([]);
  const [loading, setLoading]   = useState(true);
  const [error, setError]       = useState(false);

  // Strip directory prefix and extension for a friendlier display name
  const formatName = (filename) =>
    filename
      .replace(/^.*[\\/]/, '')   // remove any path prefix
      .replace(/\.[^/.]+$/, '')  // remove extension
      .replace(/[_-]/g, ' ');    // underscores / dashes → spaces

  const fetchTracks = useCallback(async () => {
    const files = await getAudioFiles();
    if (files.length === 0) {
      setError(true);
    }
    setTracks(files);
    setLoading(false);
  }, []);

  useEffect(() => {
    let isActive = true;
    getAudioFiles().then((files) => {
      if (!isActive) return;
      setError(files.length === 0);
      setTracks(files);
      setLoading(false);
    });

    return () => {
      isActive = false;
    };
  }, []);

  const refreshTracks = () => {
    setLoading(true);
    void fetchTracks();
  };

  const handlePlayToggle = async (filename) => {
    if (audioTrack === filename && audioPlaying) {
      // Pause
      const ok = await sendAudioPause();
      if (!ok) {
        showToast('Could not reach the slave audio player');
        return;
      }
      setAppState(prev => ({ ...prev, audioPlaying: false }));
      showToast('Audio paused');
    } else {
      // Play
      const ok = await sendAudioPlay(filename);
      if (!ok) {
        showToast('Could not play this track. Check the slave ESP32 and /DIORAMA files.');
        return;
      }
      setAppState(prev => ({ ...prev, audioPlaying: true, audioTrack: filename }));
      showToast(`Playing ${formatName(filename)}`);
    }
  };

  return (
    <div className="page-container">
      {/* Header */}
      <div className="page-header-text">
        <div className="section-breadcrumb audio-breadcrumb">LIBRARY / AUDIO</div>
        <h1 className="page-main-title">Audio</h1>
        <p className="page-subtitle">Bring a soundtrack to your little world.</p>
      </div>

      {/* SD-Card Tracks Section */}
      <div className="section-block mt-4">
        <div className="section-block-header">
          <h2 className="section-block-title">SD Card Tracks</h2>
          <button
            className="btn-icon-refresh"
            onClick={refreshTracks}
            title="Refresh track list"
          >
            <RefreshCw size={16} className={loading ? 'spin' : ''} />
          </button>
        </div>

        {loading && (
          <p className="audio-status-msg">Loading tracks from SD card…</p>
        )}

        {!loading && error && (
          <div className="audio-empty-state">
            <AlertCircle size={32} className="audio-empty-icon" />
            <p className="audio-empty-title">No tracks found</p>
            <p className="empty-subtext">
              Make sure the SD card is inserted and the ESP32 is reachable.
            </p>
          </div>
        )}

        {!loading && !error && (
          <div className="recordings-list">
            {tracks.map((filename) => {
              const isPlaying = audioTrack === filename && audioPlaying;
              return (
                <div key={filename} className="control-card recording-item">
                  <div className="rec-info-group">
                    <div className="icon-badge purple small">
                      <Music size={18} />
                    </div>
                    <div className="title-stack">
                      <h4 className="rec-name">{formatName(filename)}</h4>
                      <span className="rec-date">{filename}</span>
                    </div>
                  </div>

                  <div className="rec-actions">
                    <button
                      className="audio-play-circle-btn purple-play"
                      onClick={() => handlePlayToggle(filename)}
                    >
                      {isPlaying
                        ? <Pause size={16} />
                        : <Play size={16} className="play-icon-offset" />}
                    </button>
                  </div>
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
