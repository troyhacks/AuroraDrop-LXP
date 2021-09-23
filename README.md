# AuroraDrop v0.1

RGB Matrix Audio Visualiser for ESP32

Based on the Auroa Demo by Jason Coon.

Welcome. Work is in progress, updates may appear now and then.

Introduction

AuroraDrop creates and runs multiple random patterns based on audio input from your PC, and renders them in a
sequenced, multi-parallelized, unpredictable flow. Thrown in are a few random efects, some image manipulations
and complimentary fixed and re-active animations, also borrowed from the Jason's Auroa demo.








Notes:

AuroraDrop's audio visualisaions currently only work when your ESP32 is connected to your PC via the USB connection,
and you are running the companion windows application 'AuroraDrop Companion'. This application captures and processes
the current audio playing on your PC and passes this to the ESP32 via serial communications, which then renders the
visualisations.

https://github.com/uklooney/AuroraDropCompanion





Suggested simple hardware solution:-
   
 * ESP32 I2S Matrix Shield
 * https://github.com/witnessmenow/ESP32-i2s-Matrix-Shield
 * https://www.youtube.com/watch?v=ZiR93TmSyE0

Libraries needed:-

 * FastLED (tested with v3.4)
 * https://github.com/FastLED/FastLED

 * ESP32 HUB75 LED MATRIX PANEL DMA Display (tested with v2.0.5)
 * https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA




Based on:

 Lots, including...

 * Aurora: https://github.com/pixelmatix/aurora
 * Copyright (c) 2014 Jason Coon

 * ESP32-HUB75-MatrixPanel-I2S-DMA and Examples: https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA
 * Copyright (c) 2021 mrfaptastic
 
 * Portions of this code are adapted from "Flocking" in "The Nature of Code" by Daniel Shiffman: http://natureofcode.com/
 * Copyright (c) 2014 Daniel Shiffman
 * http://www.shiffman.net

 * Portions of this code are adapted from "Funky Clouds" by Stefan Petrick: https://gist.github.com/anonymous/876f908333cd95315c35
 * Portions of this code are adapted from "NoiseSmearing" by Stefan Petrick: https://gist.github.com/StefanPetrick/9ee2f677dbff64e3ba7a
 * Copyright (c) 2014 Stefan Petrick
 * http://www.stefan-petrick.de/wordpress_beta

 * Portions of this code are adapted from Noel Bundy's work: https://github.com/TwystNeko/Object3d
 * Copyright (c) 2014 Noel Bundy
 
 * Portions of this code are adapted from the Petty library: https://code.google.com/p/peggy/
 * Copyright (c) 2008 Windell H Oskay.  All right reserved.

* Portions of this code are adapted from SmartMatrixSwirl by Mark Kriegsman: https://gist.github.com/kriegsman/5adca44e14ad025e6d3b
* https://www.youtube.com/watch?v=bsGBT-50cts
* Copyright (c) 2014 Mark Kriegsman

* Portions of this code are adapted from LedEffects Plasma by Robert Atkins:
* https://bitbucket.org/ratkins/ledeffects/src/26ed3c51912af6fac5f1304629c7b4ab7ac8ca4b/Plasma.cpp?at=default
* Copyright (c) 2013 Robert Atkins