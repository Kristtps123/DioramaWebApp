import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.jsx'
import TFTPreview from './components/TFTPreview.jsx'

const showTFTPreview = import.meta.env.DEV &&
  new URLSearchParams(window.location.search).has('tft-preview')

createRoot(document.getElementById('root')).render(
  <StrictMode>
    {showTFTPreview ? <TFTPreview /> : <App />}
  </StrictMode>,
)
