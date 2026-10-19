import React, { useEffect, useRef } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { STLLoader } from 'three/examples/jsm/loaders/STLLoader.js';
import { getLiveSensors, getSoundLevel, getForceLevel } from '../services/esp32Api';

export default function DioramaCanvas({
  lightsOn = true,
  brightness = 75,
  lightingMode = 'Basic',
  soundReactiveOn = false,
  fountainOn = true,
  fountainStrength = 100,
  fountainAuxStrength = 75,
  fountainForceSensorOn = false,
  fountainColor = '#6bbcd9',
  fountainAuxColor = '#3B9DB3',
  circleColor = '#d9cebe',
  autoDimming = true,
  audioPlaying = false,
  onGateClick = () => { }
}) {
  const mountRef = useRef(null);
  const sceneRef = useRef(null);
  const lightsGroupRef = useRef(null);
  const fountainParticlesRef = useRef([]);
  const plazaMeshRef = useRef(null);
  const fountainMeshesRef = useRef([]);
  const gateRef = useRef(null);
  // Sound sensor data polled from ESP32
  const soundDataRef = useRef({ detected: false, level: 0 });
  const ambientDataRef = useRef({ connected: false, lux: null, brightness: null });
  // Force sensor data polled from ESP32
  const forceDataRef = useRef({ active: false, level: 0, strength: 75 });

  const propsRef = useRef({
    fountainOn,
    fountainStrength,
    fountainAuxStrength,
    fountainForceSensorOn,
    fountainColor,
    fountainAuxColor,
    circleColor,
    audioPlaying,
    lightingMode,
    soundReactiveOn,
    lightsOn,
    brightness
  });

  useEffect(() => {
    propsRef.current = {
      fountainOn,
      fountainStrength,
      fountainAuxStrength,
      fountainForceSensorOn,
      fountainColor,
      fountainAuxColor,
      circleColor,
      audioPlaying,
      lightingMode,
      soundReactiveOn,
      lightsOn,
      brightness
    };
  }, [
    fountainOn,
    fountainStrength,
    fountainAuxStrength,
    fountainForceSensorOn,
    fountainColor,
    fountainAuxColor,
    circleColor,
    audioPlaying,
    lightingMode,
    soundReactiveOn,
    lightsOn,
    brightness
  ]);

  // Poll ESP32 sound sensor every 100ms when Sound Reactive mode is active
  const isSoundReactiveMode = soundReactiveOn || lightingMode === 'Sound Reactive';
  useEffect(() => {
    if (!isSoundReactiveMode) {
      soundDataRef.current = { detected: false, level: 0 };
      return;
    }
    let cancelled = false;
    const poll = async () => {
      if (cancelled) return;
      const data = await getSoundLevel();
      if (!cancelled) soundDataRef.current = data;
      if (!cancelled) setTimeout(poll, 100);
    };
    poll();
    return () => { cancelled = true; soundDataRef.current = { detected: false, level: 0 }; };
  }, [isSoundReactiveMode]);

  useEffect(() => {
    if (lightingMode !== 'Color Adaptive') {
      ambientDataRef.current = { connected: false, lux: null, brightness: null };
      return undefined;
    }
    let cancelled = false;
    const pollAmbient = async () => {
      const data = await getLiveSensors();
      if (!cancelled && data?.ambientLight) ambientDataRef.current = data.ambientLight;
      if (!cancelled) setTimeout(pollAmbient, 3000);
    };
    pollAmbient();
    return () => {
      cancelled = true;
      ambientDataRef.current = { connected: false, lux: null, brightness: null };
    };
  }, [lightingMode]);

  // Poll ESP32 force sensor every 100ms when Force Sensor Control is active
  useEffect(() => {
    if (!fountainForceSensorOn) {
      forceDataRef.current = { active: false, level: 0, strength: 75 };
      return;
    }
    let cancelled = false;
    const poll = async () => {
      if (cancelled) return;
      const data = await getForceLevel();
      if (!cancelled) {
        if (data && data.level !== undefined) {
          forceDataRef.current = {
            active: data.active,
            level: data.level,
            strength: Math.min(100, Math.round(data.level / 10.23))
          };
        } else {
          forceDataRef.current = {
            active: false,
            level: 0,
            strength: 0
          };
        }
      }
      if (!cancelled) setTimeout(poll, 100);
    };
    poll();
    return () => {
      cancelled = true;
      forceDataRef.current = { active: false, level: 0, strength: 75 };
    };
  }, [fountainForceSensorOn]);

  useEffect(() => {
    const container = mountRef.current;
    if (!container) return;

    const width = container.clientWidth;
    const height = container.clientHeight;

    // Scene setup
    const scene = new THREE.Scene();
    scene.background = null;
    sceneRef.current = scene;

    // Camera tailored for wide diorama layout
    const camera = new THREE.PerspectiveCamera(36, width / height, 0.1, 1000);
    camera.position.set(0, 17, 23);

    // Renderer setup
    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    renderer.setSize(width, height);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = THREE.PCFShadowMap;
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.15;

    container.innerHTML = '';
    container.appendChild(renderer.domElement);

    // Orbit Controls
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.dampingFactor = 0.05;
    controls.maxPolarAngle = Math.PI / 2 - 0.02;
    controls.minDistance = 10;
    controls.maxDistance = 50;
    controls.target.set(0, 1.2, 0);

    // Lighting Group
    const lightingGroup = new THREE.Group();
    scene.add(lightingGroup);

    const ambientLight = new THREE.AmbientLight(0xfffaee, 0.85);
    lightingGroup.add(ambientLight);

    const sunLight = new THREE.DirectionalLight(0xfff8ea, 1.3);
    sunLight.position.set(15, 24, 12);
    sunLight.castShadow = true;
    sunLight.shadow.mapSize.width = 2048;
    sunLight.shadow.mapSize.height = 2048;
    sunLight.shadow.camera.near = 0.5;
    sunLight.shadow.camera.far = 60;
    sunLight.shadow.camera.left = -16;
    sunLight.shadow.camera.right = 16;
    sunLight.shadow.camera.top = 12;
    sunLight.shadow.camera.bottom = -12;
    lightingGroup.add(sunLight);

    // Dynamic Point Lights Group (Pathway Lamps & Spotlights)
    const pointLightsGroup = new THREE.Group();
    scene.add(pointLightsGroup);
    lightsGroupRef.current = pointLightsGroup;

    // --- MAIN DIORAMA ASSEMBLY ---
    const dioramaGroup = new THREE.Group();
    scene.add(dioramaGroup);

    // Dimensions matching exact reference image
    const BOX_W = 21.0;
    const BOX_D = 11.2;
    const BASE_H = 1.5;

    // 1. Sleek Black Enclosure Base Frame
    const baseGeo = new THREE.BoxGeometry(BOX_W, BASE_H, BOX_D);
    const baseMat = new THREE.MeshStandardMaterial({
      color: 0x18191b,
      roughness: 0.35,
      metalness: 0.75
    });
    const baseMesh = new THREE.Mesh(baseGeo, baseMat);
    baseMesh.position.y = -BASE_H / 2;
    baseMesh.receiveShadow = true;
    dioramaGroup.add(baseMesh);

    // Front Details: Water Input Panel, Knobs, & Power Cable Connector
    const panelMat = new THREE.MeshStandardMaterial({ color: 0x27292c, roughness: 0.4, metalness: 0.8 });
    const knobMat = new THREE.MeshStandardMaterial({ color: 0xcccccc, metalness: 0.9, roughness: 0.2 });

    // Water Input Box (Left Front)
    const waterInputBox = new THREE.Mesh(new THREE.BoxGeometry(2.4, 0.7, 0.05), panelMat);
    waterInputBox.position.set(-7.5, -BASE_H / 2, BOX_D / 2 + 0.03);
    dioramaGroup.add(waterInputBox);

    const waterKnob = new THREE.Mesh(new THREE.CylinderGeometry(0.14, 0.14, 0.15, 16), knobMat);
    waterKnob.rotation.x = Math.PI / 2;
    waterKnob.position.set(-6.8, -BASE_H / 2, BOX_D / 2 + 0.08);
    dioramaGroup.add(waterKnob);

    // Center Drawer Panel
    const drawerPanel = new THREE.Mesh(new THREE.BoxGeometry(4.8, 0.75, 0.05), panelMat);
    drawerPanel.position.set(2.0, -BASE_H / 2, BOX_D / 2 + 0.03);
    dioramaGroup.add(drawerPanel);

    const drawerKnob = new THREE.Mesh(new THREE.CylinderGeometry(0.14, 0.14, 0.15, 16), knobMat);
    drawerKnob.rotation.x = Math.PI / 2;
    drawerKnob.position.set(4.0, -BASE_H / 2, BOX_D / 2 + 0.08);
    dioramaGroup.add(drawerKnob);



    // 2. Sage Green Lawn Platform
    const grassGeo = new THREE.BoxGeometry(BOX_W - 0.4, 0.5, BOX_D - 0.4);
    const grassMat = new THREE.MeshStandardMaterial({
      color: 0x9cb096, // Sage green matching reference photo
      roughness: 0.85
    });
    const grassMesh = new THREE.Mesh(grassGeo, grassMat);
    grassMesh.position.y = 0.25;
    grassMesh.receiveShadow = true;
    dioramaGroup.add(grassMesh);

    // 3. Curved Pathway Network (Oval loop + Cross connectors)
    const pathMat = new THREE.MeshStandardMaterial({
      color: 0xd8cebe, // Warm sandy paver tone
      roughness: 0.85
    });

    // Central Horizontal Path
    const pathMainH = new THREE.Mesh(new THREE.BoxGeometry(BOX_W - 1.2, 0.02, 1.8), pathMat);
    pathMainH.position.y = 0.51;
    pathMainH.receiveShadow = true;
    dioramaGroup.add(pathMainH);

    // Central Vertical Path
    const pathMainV = new THREE.Mesh(new THREE.BoxGeometry(1.8, 0.02, BOX_D - 1.2), pathMat);
    pathMainV.position.y = 0.51;
    pathMainV.receiveShadow = true;
    dioramaGroup.add(pathMainV);

    // Outer Curved Loop Path Ribbons
    const loopRadiusX = 7.8;
    const loopRadiusZ = 3.6;
    const pathLoopGeo = new THREE.RingGeometry(loopRadiusZ - 0.6, loopRadiusZ + 0.6, 64);
    const leftLoop = new THREE.Mesh(pathLoopGeo, pathMat);
    leftLoop.rotation.x = -Math.PI / 2;
    leftLoop.position.set(-4.5, 0.51, 0);
    leftLoop.scale.set(1.4, 1.0, 1.0);
    leftLoop.receiveShadow = true;
    dioramaGroup.add(leftLoop);

    const rightLoop = new THREE.Mesh(pathLoopGeo, pathMat);
    rightLoop.rotation.x = -Math.PI / 2;
    rightLoop.position.set(4.5, 0.51, 0);
    rightLoop.scale.set(1.4, 1.0, 1.0);
    rightLoop.receiveShadow = true;
    dioramaGroup.add(rightLoop);

    // 4. Central Circular Colonnade Plaza (Matching the pillar ring in reference photo)
    const plazaRadius = 2.5;

    // Create Plaza Group for continuous rotation
    const plazaGroup = new THREE.Group();
    plazaGroup.position.set(0, 0, 0);
    dioramaGroup.add(plazaGroup);

    const plazaGeo = new THREE.CylinderGeometry(plazaRadius, plazaRadius, 0.04, 48);
    const plazaMat = new THREE.MeshStandardMaterial({
      color: new THREE.Color(circleColor),
      roughness: 0.65
    });
    const plaza = new THREE.Mesh(plazaGeo, plazaMat);
    plaza.position.y = 0.52;
    plaza.receiveShadow = true;
    plazaGroup.add(plaza);
    plazaMeshRef.current = plaza;

    // Colonnade Pillar Ring
    const pillarMat = new THREE.MeshStandardMaterial({ color: 0xc4b7a5, roughness: 0.5 });
    const pillarTopRingMat = new THREE.MeshStandardMaterial({ color: 0xb5a794, roughness: 0.5 });
    const numPillars = 20;

    for (let i = 0; i < numPillars; i++) {
      const angle = (i / numPillars) * Math.PI * 2;
      const px = Math.cos(angle) * (plazaRadius - 0.2);
      const pz = Math.sin(angle) * (plazaRadius - 0.2);

      const pillar = new THREE.Mesh(new THREE.CylinderGeometry(0.08, 0.1, 0.8, 12), pillarMat);
      pillar.position.set(px, 0.92, pz);
      pillar.castShadow = true;
      plazaGroup.add(pillar);
    }

    // Colonnade Architrave Ring
    const ringBeamGeo = new THREE.TorusGeometry(plazaRadius - 0.2, 0.1, 12, 48);
    const ringBeam = new THREE.Mesh(ringBeamGeo, pillarTopRingMat);
    ringBeam.rotation.x = Math.PI / 2;
    ringBeam.position.y = 1.34;
    ringBeam.castShadow = true;
    plazaGroup.add(ringBeam);

    // Add children positioned inside the circle in a circular formation
    const numChildren = 8;
    const childMat1 = new THREE.MeshStandardMaterial({ color: 0xef476f, roughness: 0.7 });
    const childMat2 = new THREE.MeshStandardMaterial({ color: 0x118ab2, roughness: 0.7 });
    const childSkinMat = new THREE.MeshStandardMaterial({ color: 0xffdcb3, roughness: 0.6 });
    const childRadius = 1.4;

    for (let i = 0; i < numChildren; i++) {
      const angle = (i / numChildren) * Math.PI * 2;
      const px = Math.cos(angle) * childRadius;
      const pz = Math.sin(angle) * childRadius;

      const childBody = new THREE.Mesh(new THREE.ConeGeometry(0.12, 0.4, 8), i % 2 === 0 ? childMat1 : childMat2);
      childBody.position.set(px, 0.74, pz);
      childBody.castShadow = true;

      const childHead = new THREE.Mesh(new THREE.SphereGeometry(0.09, 12, 12), childSkinMat);
      childHead.position.set(px, 1.0, pz);
      childHead.castShadow = true;

      plazaGroup.add(childBody);
      plazaGroup.add(childHead);
    }

    // 5. Dual Fountain Basins (Left & Right) + Custom STL Model
    const fountainPositions = [
      { x: -5.8, z: 0 },
      { x: 5.8, z: 0 }
    ];

    const fountainBaseMat = new THREE.MeshStandardMaterial({ color: 0x7da4b3, roughness: 0.4 });

    fountainPositions.forEach((pos, idx) => {
      const fGroup = new THREE.Group();
      fGroup.position.set(pos.x, 0.5, pos.z);

      // Outer Ring Basin Pool
      const basin = new THREE.Mesh(new THREE.CylinderGeometry(1.5, 1.3, 0.4, 24), fountainBaseMat);
      basin.position.y = 0.2;
      basin.castShadow = true;
      fGroup.add(basin);

      // Each pool gets its own material with the correct side color
      const poolColor = idx === 0 ? fountainColor : (fountainAuxColor || '#3B9DB3');
      const poolMat = new THREE.MeshStandardMaterial({
        color: new THREE.Color(poolColor),
        roughness: 0.1,
        transparent: true,
        opacity: 0.85
      });
      const pool = new THREE.Mesh(new THREE.CylinderGeometry(1.35, 1.35, 0.05, 24), poolMat);
      pool.position.y = 0.38;
      pool.userData.isLeft = idx === 0;
      fGroup.add(pool);
      fountainMeshesRef.current.push(pool);

      dioramaGroup.add(fGroup);
    });

    // Load Classmate's fountainFinal.stl onto both fountain pools
    const stlLoader = new STLLoader();
    stlLoader.load(
      '/fountainFinal.stl',
      (geometry) => {
        geometry.computeVertexNormals();
        geometry.center();

        geometry.computeBoundingBox();
        const bbox = geometry.boundingBox;
        const sizeY = bbox.max.y - bbox.min.y;
        const desiredHeight = 1.6;
        const scaleFactor = desiredHeight / (sizeY || 1);

        fountainPositions.forEach((pos, idx) => {
          const stlMat = new THREE.MeshStandardMaterial({
            color: new THREE.Color(idx === 0 ? fountainColor : (propsRef.current.fountainAuxColor || fountainColor)),
            roughness: 0.3,
            metalness: 0.2
          });

          const stlMesh = new THREE.Mesh(geometry, stlMat);
          stlMesh.rotation.x = -Math.PI / 2;
          stlMesh.scale.set(scaleFactor, scaleFactor, scaleFactor);
          stlMesh.position.set(pos.x, 1.3, pos.z);
          stlMesh.castShadow = true;
          stlMesh.receiveShadow = true;
          stlMesh.userData.isLeft = idx === 0;

          fountainMeshesRef.current.push(stlMesh);
          dioramaGroup.add(stlMesh);
        });
      },
      undefined,
      (err) => console.log('STL Load notice:', err)
    );

    // Animated Fountain Water Particle Spray
    const particles = [];
    const particleGeo = new THREE.SphereGeometry(0.06, 6, 6);

    fountainPositions.forEach((fp, fpIdx) => {
      // Each fountain side gets its own material so colors can differ
      const pColor = fpIdx === 0 ? fountainColor : (propsRef.current.fountainAuxColor || fountainColor);
      const sideMat = new THREE.MeshBasicMaterial({
        color: new THREE.Color(pColor),
        transparent: true,
        opacity: 0.85
      });

      for (let p = 0; p < 28; p++) {
        const particle = new THREE.Mesh(particleGeo, sideMat);
        particle.position.set(
          fp.x + (Math.random() - 0.5) * 0.3,
          1.5 + Math.random() * 0.7,
          fp.z + (Math.random() - 0.5) * 0.3
        );
        particle.userData = {
          originX: fp.x,
          originZ: fp.z,
          isLeft: fp.x < 0,
          vy: 0.035 + Math.random() * 0.03,
          vx: (Math.random() - 0.5) * 0.025,
          vz: (Math.random() - 0.5) * 0.025
        };
        dioramaGroup.add(particle);
        particles.push(particle);
      }
    });
    fountainParticlesRef.current = particles;

    // 6. Park Sign Plate ("We ❤️ SANTA ELENA" on right path)
    const signGroup = new THREE.Group();
    signGroup.position.set(7.5, 0.52, -1.2);

    const signBoard = new THREE.Mesh(new THREE.BoxGeometry(2.2, 0.45, 0.08), new THREE.MeshStandardMaterial({ color: 0x3d352b, roughness: 0.6 }));
    signBoard.position.y = 0.5;
    signBoard.castShadow = true;
    signGroup.add(signBoard);

    const signPost1 = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 0.6, 8), new THREE.MeshStandardMaterial({ color: 0x1f1f1f }));
    signPost1.position.set(-0.9, 0.3, 0);
    signGroup.add(signPost1);

    const signPost2 = new THREE.Mesh(new THREE.CylinderGeometry(0.03, 0.03, 0.6, 8), new THREE.MeshStandardMaterial({ color: 0x1f1f1f }));
    signPost2.position.set(0.9, 0.3, 0);
    signGroup.add(signPost2);

    // Heart Symbol Badge
    const heartMesh = new THREE.Mesh(new THREE.SphereGeometry(0.09, 8, 8), new THREE.MeshStandardMaterial({ color: 0xe63946 }));
    heartMesh.position.set(-0.25, 0.5, 0.06);
    signGroup.add(heartMesh);

    dioramaGroup.add(signGroup);

    // 7. Stone Arch Gates (Front & Back - Matching reference photo)
    const archMat = new THREE.MeshStandardMaterial({ color: 0x8c6d58, roughness: 0.65 });
    const archRoofMat = new THREE.MeshStandardMaterial({ color: 0x543f32, roughness: 0.5 });
    const ironGateMat = new THREE.MeshStandardMaterial({ color: 0x1f1f1f, metalness: 0.8 });

    const createStoneGate = (x, z, rotY, isInteractive = false) => {
      const gateGroup = new THREE.Group();
      gateGroup.position.set(x, 0.5, z);
      gateGroup.rotation.y = rotY;

      // Pillars
      const p1 = new THREE.Mesh(new THREE.BoxGeometry(0.65, 3.0, 0.65), archMat);
      p1.position.set(-1.2, 1.5, 0);
      p1.castShadow = true;
      gateGroup.add(p1);

      const p2 = new THREE.Mesh(new THREE.BoxGeometry(0.65, 3.0, 0.65), archMat);
      p2.position.set(1.2, 1.5, 0);
      p2.castShadow = true;
      gateGroup.add(p2);

      // Top Arch Beam
      const topBeam = new THREE.Mesh(new THREE.BoxGeometry(3.0, 0.5, 0.75), archRoofMat);
      topBeam.position.set(0, 3.0, 0);
      topBeam.castShadow = true;
      gateGroup.add(topBeam);

      const archCurveGeo = new THREE.TorusGeometry(1.0, 0.22, 12, 24, Math.PI);
      const archCurve = new THREE.Mesh(archCurveGeo, archRoofMat);
      archCurve.position.set(0, 2.5, 0);
      archCurve.castShadow = true;
      gateGroup.add(archCurve);

      // Inner Wrought-Iron Grill Doors
      for (let g = -0.8; g <= 0.8; g += 0.25) {
        const bar = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 2.2, 8), ironGateMat);
        bar.position.set(g, 1.2, 0);
        gateGroup.add(bar);
      }

      if (isInteractive) gateRef.current = gateGroup;
      return gateGroup;
    };

    dioramaGroup.add(createStoneGate(0, 4.8, 0, true)); // Front Gate

    // 8. Perimeter Dense Pine Trees & Shrubs (Matching reference photo dense foliage)
    const trunkMat = new THREE.MeshStandardMaterial({ color: 0x3d2b1f, roughness: 0.9 });
    const foliageMat1 = new THREE.MeshStandardMaterial({ color: 0x2b4c3b, roughness: 0.8, flatShading: true });
    const foliageMat2 = new THREE.MeshStandardMaterial({ color: 0x365d49, roughness: 0.75, flatShading: true });

    const createDenseTree = (x, z, scale = 1, isAlt = false) => {
      const tree = new THREE.Group();
      tree.position.set(x, 0.5, z);
      tree.scale.set(scale, scale, scale);

      const trunk = new THREE.Mesh(new THREE.CylinderGeometry(0.18, 0.25, 1.0, 8), trunkMat);
      trunk.position.y = 0.5;
      trunk.castShadow = true;
      tree.add(trunk);

      const mat = isAlt ? foliageMat2 : foliageMat1;

      const cone1 = new THREE.Mesh(new THREE.ConeGeometry(1.3, 1.5, 7), mat);
      cone1.position.y = 1.4;
      cone1.castShadow = true;
      tree.add(cone1);

      const cone2 = new THREE.Mesh(new THREE.ConeGeometry(1.0, 1.3, 7), mat);
      cone2.position.y = 2.1;
      cone2.castShadow = true;
      tree.add(cone2);

      const cone3 = new THREE.Mesh(new THREE.ConeGeometry(0.7, 1.0, 7), mat);
      cone3.position.y = 2.7;
      cone3.castShadow = true;
      tree.add(cone3);

      return tree;
    };

    // Dense tree row along back, left, right, and front corners
    const treePositions = [
      // Back Row
      [-9.2, -4.2, 1.15], [-7.8, -4.5, 0.9], [-6.2, -4.2, 1.2], [-4.5, -4.5, 1.0], [-2.8, -4.2, 1.1],
      [-1.4, -4.4, 1.05], [0.0, -4.5, 1.15], [1.4, -4.4, 1.05],
      [2.8, -4.2, 1.1], [4.5, -4.5, 1.0], [6.2, -4.2, 1.25], [7.8, -4.5, 0.95], [9.2, -4.2, 1.15],
      // Left Edge
      [-9.4, -2.5, 1.0], [-9.2, -0.8, 1.2], [-9.5, 0.8, 0.95], [-9.2, 2.5, 1.1], [-9.4, 4.0, 1.0],
      // Right Edge
      [9.4, -2.5, 1.0], [9.2, -0.8, 1.15], [9.5, 0.8, 1.0], [9.2, 2.5, 1.2], [9.4, 4.0, 0.9],
      // Front Row & Gate Flanking Trees
      [-7.5, 4.2, 1.1], [-5.8, 4.4, 0.95], [-4.2, 4.4, 1.05], [-2.6, 4.4, 1.0],
      [2.6, 4.4, 1.0], [4.2, 4.4, 1.05], [5.8, 4.4, 1.0], [7.5, 4.2, 1.15]
    ];
    treePositions.forEach(([tx, tz, ts], idx) => dioramaGroup.add(createDenseTree(tx, tz, ts, idx % 2 === 0)));

    // Agave / Succulent Plants along path borders
    const plantMat = new THREE.MeshStandardMaterial({ color: 0x476b56, roughness: 0.6 });
    const plantPositions = [
      [-3.2, 3.2], [-2.2, 3.6], [2.2, 3.6], [3.2, 3.2],
      [-3.2, -3.2], [-2.2, -3.6], [2.2, -3.6], [3.2, -3.2]
    ];

    plantPositions.forEach(([px, pz]) => {
      const plant = new THREE.Group();
      plant.position.set(px, 0.51, pz);
      for (let l = 0; l < 8; l++) {
        const leaf = new THREE.Mesh(new THREE.ConeGeometry(0.12, 0.7, 5), plantMat);
        leaf.rotation.x = Math.PI / 4;
        leaf.rotation.y = (l / 8) * Math.PI * 2;
        leaf.position.y = 0.2;
        plant.add(leaf);
      }
      dioramaGroup.add(plant);
    });

    // 9. Warm Pathway Bollard Lamps (Matching golden night glow along path in photo)
    const lampMat = new THREE.MeshStandardMaterial({ color: 0x222222 });
    const glowBulbMat = new THREE.MeshBasicMaterial({ color: 0xffea9f });

    const pathwayLampPositions = [
      // Along Left Loop
      [-7.8, -1.8], [-6.2, -2.8], [-4.2, -2.8], [-2.8, -1.8],
      [-7.8, 1.8], [-6.2, 2.8], [-4.2, 2.8], [-2.8, 1.8],
      // Along Right Loop
      [2.8, -1.8], [4.2, -2.8], [6.2, -2.8], [7.8, -1.8],
      [2.8, 1.8], [4.2, 2.8], [6.2, 2.8], [7.8, 1.8],
      // Along Central Plaza
      [-1.8, -0.9], [1.8, -0.9], [-1.8, 0.9], [1.8, 0.9],
      // Along Gates
      [-1.2, 4.0], [1.2, 4.0], [-1.2, -4.0], [1.2, -4.0]
    ];

    pathwayLampPositions.forEach(([lx, lz]) => {
      const lamp = new THREE.Group();
      lamp.position.set(lx, 0.5, lz);

      const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.035, 0.05, 0.55, 8), lampMat);
      pole.position.y = 0.275;
      pole.castShadow = true;
      lamp.add(pole);

      const cap = new THREE.Mesh(new THREE.CylinderGeometry(0.1, 0.1, 0.05, 8), lampMat);
      cap.position.y = 0.58;
      lamp.add(cap);

      const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.08, 8, 8), glowBulbMat);
      bulb.position.y = 0.52;
      lamp.add(bulb);

      const pLight = new THREE.PointLight(0xffea9f, (brightness / 100) * 1.2, 3.8);
      pLight.position.set(lx, 1.1, lz);
      pointLightsGroup.add(pLight);

      dioramaGroup.add(lamp);
    });

    // 10. Glass Container (Matching exact glass casing with dark top rim in photo)
    const glassWallMat = new THREE.MeshPhysicalMaterial({
      color: 0xffffff,
      transmission: 0.94,
      opacity: 1,
      transparent: true,
      roughness: 0.04,
      ior: 1.45,
      thickness: 0.25,
      specularIntensity: 0.9
    });

    const glassHeight = 6.2;
    const glassW = BOX_W - 0.2;
    const glassD = BOX_D - 0.2;

    // Glass Walls
    const glassFront = new THREE.Mesh(new THREE.BoxGeometry(glassW, glassHeight, 0.06), glassWallMat);
    glassFront.position.set(0, glassHeight / 2 + 0.5, glassD / 2);
    dioramaGroup.add(glassFront);

    const glassBack = new THREE.Mesh(new THREE.BoxGeometry(glassW, glassHeight, 0.06), glassWallMat);
    glassBack.position.set(0, glassHeight / 2 + 0.5, -glassD / 2);
    dioramaGroup.add(glassBack);

    const glassLeft = new THREE.Mesh(new THREE.BoxGeometry(0.06, glassHeight, glassD), glassWallMat);
    glassLeft.position.set(-glassW / 2, glassHeight / 2 + 0.5, 0);
    dioramaGroup.add(glassLeft);

    const glassRight = new THREE.Mesh(new THREE.BoxGeometry(0.06, glassHeight, glassD), glassWallMat);
    glassRight.position.set(glassW / 2, glassHeight / 2 + 0.5, 0);
    dioramaGroup.add(glassRight);

    // Dark Metallic Top Border Frame
    const topFrameMat = new THREE.MeshStandardMaterial({ color: 0x1f2022, roughness: 0.4, metalness: 0.8 });
    const topFrameFront = new THREE.Mesh(new THREE.BoxGeometry(glassW + 0.1, 0.15, 0.15), topFrameMat);
    topFrameFront.position.set(0, glassHeight + 0.5, glassD / 2);
    dioramaGroup.add(topFrameFront);

    const topFrameBack = new THREE.Mesh(new THREE.BoxGeometry(glassW + 0.1, 0.15, 0.15), topFrameMat);
    topFrameBack.position.set(0, glassHeight + 0.5, -glassD / 2);
    dioramaGroup.add(topFrameBack);

    const topFrameLeft = new THREE.Mesh(new THREE.BoxGeometry(0.15, 0.15, glassD + 0.1), topFrameMat);
    topFrameLeft.position.set(-glassW / 2, glassHeight + 0.5, 0);
    dioramaGroup.add(topFrameLeft);

    const topFrameRight = new THREE.Mesh(new THREE.BoxGeometry(0.15, 0.15, glassD + 0.1), topFrameMat);
    topFrameRight.position.set(glassW / 2, glassHeight + 0.5, 0);
    dioramaGroup.add(topFrameRight);

    // Raycaster User Interaction
    const raycaster = new THREE.Raycaster();
    const mouse = new THREE.Vector2();

    const handleClick = (e) => {
      const rect = renderer.domElement.getBoundingClientRect();
      mouse.x = ((e.clientX - rect.left) / rect.width) * 2 - 1;
      mouse.y = -((e.clientY - rect.top) / rect.height) * 2 + 1;
      raycaster.setFromCamera(mouse, camera);
      const intersects = raycaster.intersectObjects(dioramaGroup.children, true);
      if (intersects.length > 0) {
        onGateClick();
        if (gateRef.current) {
          const start = gateRef.current.rotation.y;
          let s = 0;
          const anim = setInterval(() => {
            s++;
            gateRef.current.rotation.y = start + Math.sin(s * 0.3) * 0.25;
            if (s > 20) {
              clearInterval(anim);
              gateRef.current.rotation.y = start;
            }
          }, 30);
        }
      }
    };
    renderer.domElement.addEventListener('click', handleClick);

    const handleResize = () => {
      if (!container) return;
      const w = container.clientWidth;
      const h = container.clientHeight;
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      renderer.setSize(w, h);
    };
    window.addEventListener('resize', handleResize);

    let frameId;
    const startAnimTime = performance.now();
    let plazaRotationAngle = 0;

    const animate = () => {
      frameId = requestAnimationFrame(animate);
      const time = (performance.now() - startAnimTime) * 0.001;

      // Gentle ambient floating rotation
      dioramaGroup.rotation.y = Math.sin(time * 0.12) * 0.04;

      const {
        lightingMode: lightMode,
        soundReactiveOn: isSoundReactive,
        lightsOn: isLightsOn,
        brightness: bLevel,
        circleColor: curCircleColor,
        fountainColor: curFountainColor,
        fountainAuxColor: curFountainAuxColor
      } = propsRef.current;

      const isSoundActive = (lightMode === 'Sound Reactive' || isSoundReactive);
      const adaptiveBrightness = lightMode === 'Color Adaptive' && ambientDataRef.current.connected
        ? ambientDataRef.current.brightness
        : bLevel;

      // Sound Reactive: lights + inner circle + fountains + water particles change RGB color
      if (isSoundActive) {
        const { detected, level, r, g, b } = soundDataRef.current;
        const soundIsActive = detected && level > 0;

        if (soundIsActive) {
          const normalizedLevel = Math.min(1, level / 1023);
          plazaRotationAngle += normalizedLevel * 0.035;

          if (isLightsOn) {
            const dynamicRgb = new THREE.Color(r / 255, g / 255, b / 255);
            if (lightsGroupRef.current) {
              const baseIntensity = (bLevel / 100) * 1.3;
              lightsGroupRef.current.children.forEach((l) => {
                if (l.color) l.color.copy(dynamicRgb);
                l.intensity = baseIntensity;
              });
            }

            if (plazaMeshRef.current && plazaMeshRef.current.material) {
              plazaMeshRef.current.material.color.copy(dynamicRgb);
            }

            if (fountainMeshesRef.current) {
              fountainMeshesRef.current.forEach((m) => {
                if (m && m.material) m.material.color.copy(dynamicRgb);
              });
            }

            if (fountainParticlesRef.current) {
              fountainParticlesRef.current.forEach((p) => {
                if (p && p.material) p.material.color.copy(dynamicRgb);
              });
            }
          }
        }
      } else {
        // Basic / Custom Color / Color Adaptive: restore colors based on mode
        const isBasic = lightMode === 'Basic';

        // Basic is steady white; other modes retain the warm scene lighting.
        if (lightsGroupRef.current) {
          const baseIntensity = isLightsOn ? (adaptiveBrightness / 100) * 1.3 : 0;
          const lightColor = new THREE.Color(isBasic ? 0xffffff : 0xffea9f);
          lightsGroupRef.current.children.forEach((l) => {
            if (l.color) l.color.copy(lightColor);
            l.intensity = baseIntensity;
          });
        }

        // Plaza (center circle)
        if (plazaMeshRef.current && plazaMeshRef.current.material) {
          const col = isBasic ? '#FFFFFF' : (curCircleColor || '#D4B78C');
          plazaMeshRef.current.material.color.set(col);
        }

        // Fountain meshes (pools + STL)
        if (fountainMeshesRef.current) {
          fountainMeshesRef.current.forEach((m) => {
            if (m && m.material) {
              let col;
              if (isBasic) {
                col = '#FFFFFF';
              } else {
                col = m.userData.isLeft
                  ? (curFountainColor || '#77898D')
                  : (curFountainAuxColor || curFountainColor || '#3B9DB3');
              }
              m.material.color.set(col);
            }
          });
        }

        // Fountain particles
        if (fountainParticlesRef.current) {
          fountainParticlesRef.current.forEach((p) => {
            if (p && p.material) {
              let col;
              if (isBasic) {
                col = '#FFFFFF';
              } else {
                col = p.userData.isLeft
                  ? (curFountainColor || '#77898D')
                  : (curFountainAuxColor || curFountainColor || '#3B9DB3');
              }
              p.material.color.set(col);
            }
          });
        }
      }
      plazaGroup.rotation.y = plazaRotationAngle;

      // Fountain Particle Physics
      if (fountainParticlesRef.current) {
        const isForceActive = propsRef.current.fountainForceSensorOn;
        const forceSpeed = (forceDataRef.current.strength / 100);
        const leftSpeed = (propsRef.current.fountainStrength / 100);
        const rightSpeed = (propsRef.current.fountainAuxStrength / 100);

        fountainParticlesRef.current.forEach((p) => {
          if (!propsRef.current.fountainOn) {
            p.visible = false;
            return;
          }
          p.visible = true;
          const currentSpeed = (isForceActive ? forceSpeed : (p.userData.isLeft ? leftSpeed : rightSpeed));
          p.position.y += p.userData.vy * currentSpeed;
          p.position.x += p.userData.vx * currentSpeed;
          p.position.z += p.userData.vz * currentSpeed;
          p.userData.vy -= 0.0015;

          if (p.position.y < 1.0) {
            p.position.set(
              p.userData.originX + (Math.random() - 0.5) * 0.25,
              1.4,
              p.userData.originZ + (Math.random() - 0.5) * 0.25
            );
            p.userData.vy = (0.035 + Math.random() * 0.03) * Math.max(0.4, currentSpeed);
          }
        });
      }

      controls.update();
      renderer.render(scene, camera);
    };

    animate();

    return () => {
      window.removeEventListener('resize', handleResize);
      if (renderer.domElement) renderer.domElement.removeEventListener('click', handleClick);
      cancelAnimationFrame(frameId);
      renderer.dispose();
    };
  }, []);

  // Sync Lights Intensity
  useEffect(() => {
    if (!lightsGroupRef.current) return;
    const target = lightsOn ? (brightness / 100) * 1.3 : 0;
    lightsGroupRef.current.children.forEach((l) => (l.intensity = target));
  }, [lightsOn, brightness]);

  // Sync Plaza (Center) Color
  useEffect(() => {
    if (plazaMeshRef.current && plazaMeshRef.current.material) {
      plazaMeshRef.current.material.color.set(circleColor || '#D4B78C');
    }
  }, [circleColor]);

  // Sync Left Fountain Color
  useEffect(() => {
    if (!fountainMeshesRef.current.length) return;
    fountainMeshesRef.current.forEach((m) => {
      if (m && m.material && m.userData.isLeft) {
        m.material.color.set(fountainColor || '#77898D');
      }
    });
    fountainParticlesRef.current.forEach((p) => {
      if (p && p.material && p.userData.isLeft) {
        p.material.color.set(fountainColor || '#77898D');
      }
    });
  }, [fountainColor]);

  // Sync Right Fountain Color
  useEffect(() => {
    if (!fountainMeshesRef.current.length) return;
    fountainMeshesRef.current.forEach((m) => {
      if (m && m.material && m.userData.isLeft === false) {
        m.material.color.set(fountainAuxColor || '#3B9DB3');
      }
    });
    fountainParticlesRef.current.forEach((p) => {
      if (p && p.material && p.userData.isLeft === false) {
        p.material.color.set(fountainAuxColor || '#3B9DB3');
      }
    });
  }, [fountainAuxColor]);

  return (
    <div className="diorama-preview-card">
      <div className="canvas-wrapper" ref={mountRef}></div>
      <div className="canvas-overlay-instruction">Live 3D preview · drag to rotate</div>
    </div>
  );
}
