import { useState } from 'react';
import {
  MapPin, Landmark, Cpu, Droplets, Lightbulb, Music, Fingerprint,
  RotateCcw, BookOpen, Calendar, User, TreePine, Star, ChevronDown
} from 'lucide-react';
import './About.css';

const features = [
  { icon: <Lightbulb size={20} />, title: 'Adaptive Lighting', desc: 'RGB LED lights with basic white, custom color, sound-reactive, and ambient-adaptive modes.' },
  { icon: <Droplets size={20} />, title: 'Four-Pump Water System', desc: 'Two pumps power the fountain, one pump drains water from the tank, and a fourth pump returns water to the reservoir.' },
  { icon: <RotateCcw size={20} />, title: 'Rotating Inner Plaza', desc: 'The central colonnade plaza can rotate continuously, on demand, or only when the mic detects sound.' },
  { icon: <Music size={20} />, title: 'Ambient Audio', desc: 'Pre-recorded soundscapes play directly into the diorama, bringing the miniature world to life.' },
  { icon: <Fingerprint size={20} />, title: 'Biometric Sensor Access', desc: 'A registered fingerprint on the biometric sensor is required to unlock the gate and access and interact with the diorama features.' },
  { icon: <Cpu size={20} />, title: 'ESP32 Sensor Hub', desc: 'Powered by an ESP32 microcontroller integrating RGB, electret mic, ambient light, color, and force sensors.' }
];

const historyTimeline = [
  { year: 'Early 1900s', icon: <TreePine size={16} />, title: 'Land Established', desc: 'The land in the heart of Sta. Elena, Camarines Norte was designated as a public green space to serve the growing community as the town developed.' },
  { year: '1940s', icon: <Star size={16} />, title: 'Named After Silvestre del Moro', desc: 'The park was officially named in honor of Silvestre del Moro, a distinguished and respected local leader who made significant contributions to the municipality of Sta. Elena and the broader Camarines Norte province.' },
  { year: '1960s-1980s', icon: <Landmark size={16} />, title: 'Development & Beautification', desc: 'Major beautification efforts transformed the park: the central water fountain was built, ornamental archway gates were erected, and walking plazas with decorative colonnade structures were added, giving the park its iconic look.' },
  { year: '1990s', icon: <User size={16} />, title: 'Community Heart', desc: 'The park became the de facto town square — a gathering place for festivals, civic events, and daily recreation. Local government units further maintained and expanded the grounds to accommodate residents.' },
  { year: '2000s-Present', icon: <Calendar size={16} />, title: 'Preserved Landmark', desc: "Silvestre del Moro Park continues to be lovingly maintained by the local government and community. It remains a symbol of Sta. Elena's heritage and civic pride, visited daily by residents and tourists alike." }
];

/* ── Accordion wrapper ─────────────────────────────────────── */
function Accordion({ icon, title, subtitle, defaultOpen = false, children }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <div className={`accordion-card ${open ? 'accordion-open' : ''}`}>
      <button className="accordion-trigger" onClick={() => setOpen(o => !o)} aria-expanded={open}>
        <div className="accordion-trigger-left">
          <div className="icon-badge sage">{icon}</div>
          <div className="title-stack">
            <span className="card-title">{title}</span>
            {subtitle && <span className="card-status-subtext">{subtitle}</span>}
          </div>
        </div>
        <ChevronDown size={20} className={`accordion-chevron ${open ? 'rotated' : ''}`} />
      </button>
      <div className="accordion-body" style={{ display: open ? 'block' : 'none' }}>
        <div className="accordion-body-inner">
          {children}
        </div>
      </div>
    </div>
  );
}

/* ── Page ──────────────────────────────────────────────────── */
export default function AboutPage({ setActiveTab }) {
  return (
    <div className="page-container about-page">

      {/* Hero */}
      <div className="about-hero">
        <div className="about-hero-badge">ABOUT THIS DIORAMA</div>
        <h1 className="about-title">Silvestre del Moro Park</h1>
        <div className="about-location-row">
          <MapPin size={16} className="about-pin-icon" />
          <span>Sta. Elena, Camarines Norte, Philippines</span>
        </div>
        <p className="about-tagline">
          A miniature park brought to life — lights, water, sound, and sensors working in harmony
          to recreate the iconic Silvestre del Moro Park in stunning detail.
        </p>
      </div>

      {/* 1. About the Park */}
      <Accordion
        id="about-park"
        icon={<Landmark size={22} />}
        title="About the Park"
        subtitle="Sta. Elena, Camarines Norte"
        defaultOpen={true}
      >
        <p className="about-body-text">
          Silvestre del Moro Park is a beloved public landmark in Sta. Elena, Camarines Norte, Philippines.
          Named in honor of a distinguished local figure, the park serves as a gathering place for the
          community — featuring lush greenery, a central water fountain, a decorated walking plaza, and
          ornamental archway gates that welcome visitors.
        </p>
        <p className="about-body-text mt-3">
          This diorama project faithfully recreates the park in miniature scale, complete with working
          LED lighting, real water flow, ambient audio, and sensor-driven automation — controlled entirely
          through this web application and an onboard ESP32 interface.
        </p>
      </Accordion>

      {/* 2. History */}
      <Accordion
        id="history"
        icon={<BookOpen size={22} />}
        title="History of the Park"
        subtitle="The story behind Silvestre del Moro Park"
      >
        {/* Founder */}
        <div className="about-founder-card">
          <div className="about-founder-avatar">
            <User size={28} />
          </div>
          <div className="about-founder-info">
            <span className="about-founder-label">Named After</span>
            <h3 className="about-founder-name">Silvestre del Moro</h3>
            <p className="about-founder-desc">
              A distinguished and respected leader of Sta. Elena, Camarines Norte, Philippines.
              Silvestre del Moro was celebrated for his invaluable service and dedication to the
              progress of his municipality. The park was named in his honor as a lasting tribute
              to his legacy and contributions to the community.
            </p>
          </div>
        </div>

        {/* Timeline */}
        <div className="about-timeline">
          {historyTimeline.map((item, i) => (
            <div key={i} className="about-timeline-item">
              <div className="about-timeline-left">
                <div className="about-timeline-dot">{item.icon}</div>
                {i < historyTimeline.length - 1 && <div className="about-timeline-line" />}
              </div>
              <div className="about-timeline-content">
                <span className="about-timeline-year">{item.year}</span>
                <h4 className="about-timeline-event">{item.title}</h4>
                <p className="about-timeline-desc">{item.desc}</p>
              </div>
            </div>
          ))}
        </div>
      </Accordion>

      {/* 3. Diorama Features */}
      <Accordion
        id="features"
        icon={<Lightbulb size={22} />}
        title="Diorama Features"
        subtitle="What this miniature can do"
      >
        <div className="about-features-grid">
          {features.map((f, i) => (
            <div key={i} className="about-feature-card">
              <div className="about-feature-icon">{f.icon}</div>
              <div>
                <h4 className="about-feature-title">{f.title}</h4>
                <p className="about-feature-desc">{f.desc}</p>
              </div>
            </div>
          ))}
        </div>
      </Accordion>

      {/* 4. Hardware & Technology */}
      <Accordion
        id="tech"
        icon={<Cpu size={22} />}
        title="Hardware & Technology"
        subtitle="Components powering the diorama"
      >
        <div className="about-tech-grid">
          {[
            ['Microcontroller', 'ESP32'],
            ['Lighting', 'RGB LED Modules'],
            ['Sound', 'Electret Microphone Sensor'],
            ['Ambient Light', 'VEML7700 on I2C'],
            ['Color Input', 'RGB Sensor Controller over UART'],
            ['Color Control', 'Manual RGB Color Picker'],
            ['Water Pump', 'Submersible DC Pump'],
            ['Pressure', 'Force Sensor (FSR)'],
            ['Gate Control', 'RFID RC522 Reader'],
            ['Control App', 'React + Vite (Web)'],
          ].map(([label, value]) => (
            <div key={label} className="about-tech-row">
              <span className="about-tech-label">{label}</span>
              <span className="about-tech-value">{value}</span>
            </div>
          ))}
        </div>
      </Accordion>

      {/* Footer CTA */}
      <div className="about-footer-cta">
        <p>Explore and control your little world.</p>
        <button className="btn-about-cta" onClick={() => setActiveTab('home')}>
          Go to Dashboard
        </button>
      </div>

    </div>
  );
}
