import { useEffect, useState, useCallback } from 'react';
import { Music, Play, Pause, Square, RefreshCw, AlertCircle, SkipBack, SkipForward } from 'lucide-react';
import {
  getAudioFiles,
  sendAudioPlay,
  sendAudioPause,
  sendAudioNext,
  sendAudioPrevious,
  sendAudioStop
} from '../../services/esp32Api';
import './Audio.css';

export default function Audio({ appState, setAppState, showToast }) {
  const { audioPlaying, audioTrack } = appState;

  const [tracks, setTracks]     = useState([]);
  const [loading, setLoading]   = useState(true);
  const [error, setError]       = useState(false);
  const [loadFailed, setLoadFailed] = useState(false);

  // Strip directory prefix and extension for a friendlier display name
  const formatName = (filename) =>
    filename
      .replace(/^.*[\\/]/, '')   // remove any path prefix
      .replace(/\.[^/.]+$/, '')  // remove extension
      .replace(/[_-]/g, ' ');    // underscores / dashes → spaces

  const fetchTracks = useCallback(async () => {
    const files = await getAudioFiles();
    if (files === null) {
      setLoadFailed(true);
      setError(true);
    } else {
      setTracks(files);
      setError(files.length === 0);
      setLoadFailed(false);
    }
    setLoading(false);
  }, []);

  useEffect(() => {
    let isActive = true;
    getAudioFiles().then((files) => {
      if (!isActive) return;
      if (files === null) {
        setLoadFailed(true);
        setError(true);
      } else {
        setTracks(files);
        setError(files.length === 0);
      }
      setLoading(false);
    });

    return () => {
      isActive = false;
    };
  }, []);

  const refreshTracks = () => {
    setLoading(true);
    setLoadFailed(false);
    void fetchTracks();
  };

  const playTrack = async (filename) => {
    const ok = await sendAudioPlay(filename);
    if (!ok) {
      showToast('Could not play this track. Check the slave ESP32 and /DIORAMA files.');
      return false;
    }
    setAppState(prev => ({ ...prev, audioPlaying: true, audioTrack: filename }));
    showToast(`Playing ${formatName(filename)}`);
    return true;
  };

  const handlePlayToggle = async (filename) => {
    if (audioTrack === filename && audioPlaying) {
      const ok = await sendAudioPause();
      if (!ok) {
        showToast('Could not reach the slave audio player');
        return;
      }
      setAppState(prev => ({ ...prev, audioPlaying: false }));
      showToast('Audio paused');
      return;
    }
    await playTrack(filename);
  };

  const stepTrack = async (direction) => {
    if (!tracks.length) return;
    const currentIndex = tracks.indexOf(audioTrack);
    const nextIndex = currentIndex < 0
      ? (direction > 0 ? 0 : tracks.length - 1)
      : (currentIndex + direction + tracks.length) % tracks.length;
    const sendStep = direction > 0 ? sendAudioNext : sendAudioPrevious;
    const ok = await sendStep();
    if (!ok) {
      showToast('Could not change track. Check the main and slave ESP32 connections.');
      return;
    }
    setAppState(prev => ({
      ...prev,
      audioPlaying: true,
      audioTrack: tracks[nextIndex]
    }));
    showToast(`Playing ${formatName(tracks[nextIndex])}`);
  };

  const togglePlayback = async () => {
    const selectedTrack = tracks.includes(audioTrack) ? audioTrack : tracks[0];
    if (!selectedTrack) return;
    if (audioPlaying) {
      const ok = await sendAudioPause();
      if (!ok) {
        showToast('Could not reach the slave audio player');
        return;
      }
      setAppState(prev => ({ ...prev, audioPlaying: false }));
      showToast('Audio paused');
      return;
    }
    await playTrack(selectedTrack);
  };

  const stopPlayback = async () => {
    const ok = await sendAudioStop();
    if (!ok) {
      showToast('Could not stop audio on the slave player');
      return;
    }
    setAppState(prev => ({
      ...prev,
      audioPlaying: false,
      audioTrack: 'No audio selected'
    }));
    showToast('Audio stopped');
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
          <div className="audio-library-actions">
            <button
              className="btn-icon-refresh"
              onClick={refreshTracks}
              title="Refresh track list"
              aria-label="Refresh track list"
            >
              <RefreshCw size={16} className={loading ? 'spin' : ''} />
            </button>
          </div>
        </div>

        <div className="audio-playback-controls" aria-label="Audio playback controls">
          <button
            className="btn-icon-refresh"
            onClick={() => void stepTrack(-1)}
            title="Play previous track"
            aria-label="Play previous track"
            disabled={tracks.length === 0}
          >
            <SkipBack size={18} />
          </button>
          <button
            className="audio-main-toggle"
            onClick={() => void togglePlayback()}
            title={audioPlaying ? 'Pause audio' : 'Play selected track'}
            aria-label={audioPlaying ? 'Pause audio' : 'Play selected track'}
            disabled={tracks.length === 0}
          >
            {audioPlaying
              ? <Pause size={19} />
              : <Play size={19} className="play-icon-offset" />}
            <span>{audioPlaying ? 'Pause' : 'Play'}</span>
          </button>
          <button
            className="btn-icon-refresh"
            onClick={() => void stepTrack(1)}
            title="Play next track"
            aria-label="Play next track"
            disabled={tracks.length === 0}
          >
            <SkipForward size={18} />
          </button>
          <button
            className="btn-icon-refresh"
            onClick={() => void stopPlayback()}
            title="Stop audio"
            aria-label="Stop audio"
            disabled={!audioTrack || audioTrack === 'No audio selected'}
          >
            <Square size={16} />
          </button>
        </div>

        {loading && tracks.length === 0 && (
          <p className="audio-status-msg">Loading tracks from SD card…</p>
        )}

        {!loading && tracks.length === 0 && error && (
          <div className="audio-empty-state">
            <AlertCircle size={32} className="audio-empty-icon" />
            <p className="audio-empty-title">
              {loadFailed ? 'Could not load tracks' : 'No tracks found'}
            </p>
            <p className="empty-subtext">
              {loadFailed
                ? 'Check the ESP32 connection and refresh the track list.'
                : 'Make sure the SD card is inserted and contains MP3 or WAV files in /DIORAMA.'}
            </p>
          </div>
        )}

        {tracks.length > 0 && (
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
