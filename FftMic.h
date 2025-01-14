/*
 * This code has been taken from the sound reactive fork of WLED by Andrew
 * Tuline, and the analog audio processing has been (mostly) removed as we
 * will only be using the INMP441.
 * 
 * The FFT code runs on core 0 while everything else runs on core 1. This
 * means we can make our main code more complex without affecting the FFT
 * processing.
 */

void automatic_binner(int steps, byte binarray[], int binstart=3, int binend=205);

#include <driver/i2s.h>

#define UM_AUDIOREACTIVE_USE_NEW_FFT

#define MIN_SHOW_DELAY 20

const i2s_port_t I2S_PORT = I2S_NUM_0;

const uint16_t samples = 512;    // Samples in an FFT batch - This value MUST ALWAYS be a power of 2
constexpr int BLOCK_SIZE = 128; 

#define I2S_SAMPLE_RESOLUTION I2S_BITS_PER_SAMPLE_32BIT
#define I2S_datatype int32_t
#define I2S_unsigned_datatype uint32_t
#define I2S_data_size I2S_BITS_PER_CHAN_32BIT
#define I2S_SAMPLE_DOWNSCALE_TO_16BIT

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)) && (ESP_IDF_VERSION <= ESP_IDF_VERSION_VAL(4, 4, 3))

    // espressif bug: only_left has no sound, left and right are swapped 
    // https://github.com/espressif/esp-idf/issues/9635  I2S mic not working since 4.4 (IDFGH-8138)
    // https://github.com/espressif/esp-idf/issues/8538  I2S channel selection issue? (IDFGH-6918)
    // https://github.com/espressif/esp-idf/issues/6625  I2S: left/right channels are swapped for read (IDFGH-4826)

    #ifdef I2S_USE_RIGHT_CHANNEL

        #define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
        #define I2S_MIC_CHANNEL_TEXT "right channel only (work-around swapped channel bug in IDF 4.4)."
        #define I2S_PDM_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_RIGHT
        #define I2S_PDM_MIC_CHANNEL_TEXT "right channel only"

    #else

        #define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_RIGHT
        #define I2S_MIC_CHANNEL_TEXT "left channel only (work-around swapped channel bug in IDF 4.4)."
        #define I2S_PDM_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
        #define I2S_PDM_MIC_CHANNEL_TEXT "left channel only."

    #endif

#else

    #ifdef I2S_USE_RIGHT_CHANNEL

        #define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_RIGHT
        #define I2S_MIC_CHANNEL_TEXT "right channel only."

    #else

        #define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT
        #define I2S_MIC_CHANNEL_TEXT "left channel only."

    #endif

    #define I2S_PDM_MIC_CHANNEL I2S_MIC_CHANNEL
    #define I2S_PDM_MIC_CHANNEL_TEXT I2S_MIC_CHANNEL_TEXT

#endif

int8_t _myADCchannel = 0x0F;  

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
    #define SRate_t uint32_t
#else
    #define SRate_t int
#endif

///////////////////////////////////
// globals
static uint8_t inputLevel = 128;              // UI slider value
#ifndef SR_SQUELCH
    uint8_t soundSquelch = 10;                  // squelch value for volume reactive routines (config value)
#else
    uint8_t soundSquelch = SR_SQUELCH;          // squelch value for volume reactive routines (config value)
#endif
#ifndef SR_GAIN
    uint8_t sampleGain = 1;                    // sample gain (config value)
#else
    uint8_t sampleGain = SR_GAIN;               // sample gain (config value)
#endif
static uint8_t soundAgc = 2;                  // Automagic gain control: 0 - none, 1 - normal, 2 - vivid, 3 - lazy (config value)
static uint8_t audioSyncEnabled = 0;          // bit field: bit 0 - send, bit 1 - receive (config value)
static bool udpSyncConnected = false;         // UDP connection status -> true if connected to multicast group

// user settable parameters for limitSoundDynamics()
static bool limiterOn = false;                 // bool: enable / disable dynamics limiter
static uint16_t attackTime =  30;             // int: attack time in milliseconds. Default 0.08sec
static uint16_t decayTime = 1100;             // int: decay time in milliseconds.  Default 1.40sec
// user settable options for FFTResult scaling
static uint8_t FFTScalingMode = 2;            // 0 none; 1 optimized logarithmic ("Loudness"); 2 optimized linear ("Amplitude"); 3 optimized sqare root ("Energy")

// 
// AGC presets
//  Note: in C++, "const" implies "static" - no need to explicitly declare everything as "static const"
// 
#define AGC_NUM_PRESETS 3 // AGC presets:          normal,   vivid,    lazy
const double agcSampleDecay[AGC_NUM_PRESETS]  = { 0.9994f, 0.9985f, 0.9997f}; // decay factor for sampleMax, in case the current sample is below sampleMax
const float agcZoneLow[AGC_NUM_PRESETS]       = {      32,      28,      36}; // low volume emergency zone
const float agcZoneHigh[AGC_NUM_PRESETS]      = {     240,     240,     248}; // high volume emergency zone
const float agcZoneStop[AGC_NUM_PRESETS]      = {     336,     448,     304}; // disable AGC integrator if we get above this level
const float agcTarget0[AGC_NUM_PRESETS]       = {     112,     144,     164}; // first AGC setPoint -> between 40% and 65%
const float agcTarget0Up[AGC_NUM_PRESETS]     = {      88,      64,     116}; // setpoint switching value (a poor man's bang-bang)
const float agcTarget1[AGC_NUM_PRESETS]       = {     220,     224,     216}; // second AGC setPoint -> around 85%
const double agcFollowFast[AGC_NUM_PRESETS]   = { 1/192.f, 1/128.f, 1/256.f}; // quickly follow setpoint - ~0.15 sec
const double agcFollowSlow[AGC_NUM_PRESETS]   = {1/6144.f,1/4096.f,1/8192.f}; // slowly follow setpoint  - ~2-15 secs
const double agcControlKp[AGC_NUM_PRESETS]    = {    0.6f,    1.5f,   0.65f}; // AGC - PI control, proportional gain parameter
const double agcControlKi[AGC_NUM_PRESETS]    = {    1.7f,   1.85f,    1.2f}; // AGC - PI control, integral gain parameter
const float agcSampleSmooth[AGC_NUM_PRESETS]  = {  1/12.f,   1/6.f,  1/16.f}; // smoothing factor for sampleAgc (use rawSampleAgc if you want the non-smoothed value)
// AGC presets end

static volatile bool disableSoundProcessing = false;      // if true, sound processing (FFT, filters, AGC) will be suspended. "volatile" as its shared between tasks.
static bool useBandPassFilter = true;                    // if true, enables a bandpass filter 80Hz-16Khz to remove noise. Applies before FFT.

// audioreactive variables shared with FFT task
static float    micDataReal = 0.0f;             // MicIn data with full 24bit resolution - lowest 8bit after decimal point
static float    multAgc = 1.0f;                 // sample * multAgc = sampleAgc. Our AGC multiplier
static float    sampleAvg = 0.0f;               // Smoothed Average sample - sampleAvg < 1 means "quiet" (simple noise gate)
static float    sampleAgc = 0.0f;               // Smoothed AGC sample

// peak detection
static bool samplePeak = false;      // Boolean flag for peak - used in effects. Responding routine may reset this flag. Auto-reset after strip.getMinShowDelay()
static uint8_t maxVol = 10;          // Reasonable value for constant volume for 'peak detector', as it won't always trigger (deprecated)
static uint8_t binNum = 8;           // Used to select the bin for FFT based beat detection  (deprecated)
static bool udpSamplePeak = false;   // Boolean flag for peak. Set at the same tiem as samplePeak, but reset by transmitAudioData
static unsigned long timeOfPeak = 0; // time of last sample peak detection.
static void detectSamplePeak(void);  // peak detection function (needs scaled FFT reasults in vReal[])
static void autoResetPeak(void);     // peak auto-reset function

////////////////////
// Begin FFT Code //
////////////////////

// some prototypes, to ensure consistent interfaces
static float mapf(float x, float in_min, float in_max, float out_min, float out_max); // map function for float
static float fftAddAvg(int from, int to);   // average of several FFT result bins
void FFTcode(void * parameter);      // audio processing task: read samples, run FFT, fill GEQ channels from FFT results
static void runMicFilter(uint16_t numSamples, float *sampleBuffer);          // pre-filtering of raw samples (band-pass)
static void postProcessFFTResults(bool noiseGateOpen, int numberOfChannels); // post-processing and post-amp of GEQ channels

#define NUM_GEQ_CHANNELS 16                                           // number of frequency channels. Don't change !!

static TaskHandle_t FFT_Task = nullptr;

// Table of multiplication factors so that we can even out the frequency response.
//
// static float fftResultPink[NUM_GEQ_CHANNELS] = { 1.70f, 1.71f, 1.73f, 1.78f, 1.68f, 1.56f, 1.55f, 1.63f, 1.79f, 1.62f, 1.80f, 2.06f, 2.47f, 3.35f, 6.83f, 9.55f };

// MoonModules:  #10 almost FLAT (IMNP441 but no PINK noise adjustments)
//
static float fftResultPink[NUM_GEQ_CHANNELS] = { 2.38f, 2.18f, 2.07f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.70f, 1.95f, 1.70f, 2.13f, 2.47f };

// globals and FFT Output variables shared with animations
static float FFT_MajorPeak = 1.0f;              // FFT: strongest (peak) frequency
static float FFT_Magnitude = 0.0f;              // FFT: volume (magnitude) of peak frequency
static uint8_t fftResult[NUM_GEQ_CHANNELS]= {0};// Our calculated freq. channel result table to be used by effects
static uint8_t AD_fftResult[NUM_GEQ_CHANNELS]= {0}; // AuroraDrop needs one faster, but leave the original one alone.

#if defined(WLED_DEBUG) || defined(SR_DEBUG)
    static uint64_t fftTime = 0;
    static uint64_t sampleTime = 0;
#endif

// FFT Task variables (filtering and post-processing)
static float   fftCalc[NUM_GEQ_CHANNELS] = {0.0f};                    // Try and normalize fftBin values to a max of 4096, so that 4096/16 = 256.
static float   fftAvg[NUM_GEQ_CHANNELS] = {0.0f};                     // Calculated frequency channel results, with smoothing (used if dynamics limiter is ON)
#ifdef SR_DEBUG
    static float   fftResultMax[NUM_GEQ_CHANNELS] = {0.0f};               // A table used for testing to determine how our post-processing is working.
#endif

// audio source parameters and constant
constexpr SRate_t SAMPLE_RATE = 22050;          // Base sample rate in Hz - 22Khz is a standard rate. Physical sample time -> 23ms
//constexpr SRate_t SAMPLE_RATE = 16000;        // 16kHz - use if FFTtask takes more than 20ms. Physical sample time -> 32ms
//constexpr SRate_t SAMPLE_RATE = 20480;        // Base sample rate in Hz - 20Khz is experimental.    Physical sample time -> 25ms
//constexpr SRate_t SAMPLE_RATE = 10240;        // Base sample rate in Hz - previous default.         Physical sample time -> 50ms
#define FFT_MIN_CYCLE 21                        // minimum time before FFT task is repeated. Use with 22Khz sampling
//#define FFT_MIN_CYCLE 30                      // Use with 16Khz sampling
//#define FFT_MIN_CYCLE 23                      // minimum time before FFT task is repeated. Use with 20Khz sampling
//#define FFT_MIN_CYCLE 46                      // minimum time before FFT task is repeated. Use with 10Khz sampling

// FFT Constants
constexpr uint16_t samplesFFT = 512;            // Samples in an FFT batch - This value MUST ALWAYS be a power of 2
constexpr uint16_t samplesFFT_2 = 256;          // meaningfull part of FFT results - only the "lower half" contains useful information.
// the following are observed values, supported by a bit of "educated guessing"
//#define FFT_DOWNSCALE 0.65f                   // 20kHz - downscaling factor for FFT results - "Flat-Top" window @20Khz, old freq channels 
#define FFT_DOWNSCALE 0.60f                     // downscaling factor for FFT results - for "Flat-Top" window @22Khz, new freq channels
#define LOG_256  5.54517744f                    // log(256)

// These are the input and output vectors.  Input vectors receive computed results from FFT.
static float vReal[samplesFFT] = {0.0f};       // FFT sample inputs / freq output -  these are our raw result bins
static float vImag[samplesFFT] = {0.0f};       // imaginary parts

#ifdef UM_AUDIOREACTIVE_USE_NEW_FFT

    static float windowWeighingFactors[samplesFFT] = {0.0f};

#endif

// Create FFT object
//
#ifdef UM_AUDIOREACTIVE_USE_NEW_FFT

    // lib_deps += https://github.com/kosme/arduinoFFT#develop @ 1.9.2
    #define FFT_SPEED_OVER_PRECISION     // enables use of reciprocals (1/x etc), and an a few other speedups
    #define FFT_SQRT_APPROXIMATION       // enables "quake3" style inverse sqrt
    #define sqrt(x) sqrtf(x)             // little hack that reduces FFT time by 50% on ESP32 (as alternative to FFT_SQRT_APPROXIMATION)

#else

    // lib_deps += https://github.com/blazoncek/arduinoFFT.git

#endif

#include <arduinoFFT.h>

#ifdef UM_AUDIOREACTIVE_USE_NEW_FFT

    static ArduinoFFT<float> FFT = ArduinoFFT<float>( vReal, vImag, samplesFFT, SAMPLE_RATE, windowWeighingFactors);

#else

    static arduinoFFT FFT = arduinoFFT(vReal, vImag, samplesFFT, SAMPLE_RATE);

#endif

// Helper functions

// float version of map()
//
static float mapf(float x, float in_min, float in_max, float out_min, float out_max) {

    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;

}

// compute average of several FFT result bins
//
static float fftAddAvg(int from, int to) {

    float result = 0.0f;

    for (int i = from; i <= to; i++) {

        result += vReal[i];

    }

    return result / float(to - from + 1);

}

// set your config variables to their boot default value (this can also be done in readFromConfig() or a constructor if you prefer)
bool     enabled = false;
bool     initDone = false;

// variables  for UDP sound sync
// WiFiUDP fftUdp;               // UDP object for sound sync (from WiFi UDP, not Async UDP!) 
unsigned long lastTime = 0;   // last time of running UDP Microphone Sync
const uint16_t delayMs = 10;  // I don't want to sample too often and overload WLED
uint16_t audioSyncPort= 11988;// default port for UDP sound sync

// used for AGC
int      last_soundAgc = -1;   // used to detect AGC mode change (for resetting AGC internal error buffers)
double   control_integrated = 0.0;   // persistent across calls to agcAvg(); "integrator control" = accumulated error

// variables used by getSample() and agcAvg()
int16_t  micIn = 0;           // Current sample starts with negative values and large values, which is why it's 16 bit signed
double   sampleMax = 0.0;     // Max sample over a few seconds. Needed for AGC controler.
double   micLev = 0.0;        // Used to convert returned value to have '0' as minimum. A leveller
float    expAdjF = 0.0f;      // Used for exponential filter.
float    sampleReal = 0.0f;	  // "sampleRaw" as float, to provide bits that are lost otherwise (before amplification by sampleGain or inputLevel). Needed for AGC.
int16_t  sampleRaw = 0;       // Current sample. Must only be updated ONCE!!! (amplified mic value by sampleGain and inputLevel)
int16_t  rawSampleAgc = 0;    // not smoothed AGC sample

// variables used in effects
float   volumeSmth = 0.0f;    // either sampleAvg or sampleAgc depending on soundAgc; smoothed sample
int16_t  volumeRaw = 0;       // either sampleRaw or rawSampleAgc depending on soundAgc
float my_magnitude =0.0f;     // FFT_Magnitude, scaled by multAgc

// used to feed "Info" Page
unsigned long last_UDPTime = 0;    // time of last valid UDP sound sync datapacket
int receivedFormat = 0;            // last received UDP sound sync format - 0=none, 1=v1 (0.13.x), 2=v2 (0.14.x)
float maxSample5sec = 0.0f;        // max sample (after AGC) in last 5 seconds 
unsigned long sampleMaxTimer = 0;  // last time maxSample5sec was reset
#define CYCLE_SAMPLEMAX 3500       // time window for merasuring

///////////////////////////////////

int squelch = 2;    // was 0                       // Squelch, cuts out low level sounds
float gain = 0;      // was 30                       // Gain, boosts input level*/
float gain_max = 60; // after a certain point I think we're boosting room tone. :D

uint16_t micData;                               // Analog input for FFT

// These are the input and output vectors.  Input vectors receive computed results from FFT.
//
// double vReal[samples];
// double vImag[samples];
double fftBin[samples];

int specDataMinVolume = 128;
int specDataMaxVolume = 0;
float mean_int = 425.0; // just below 100 bpm in ms

// BPM Test Stuff:
//
float magAvg = 64;
float lastBeat = 0;  // time of last beat in millis()
float bpm_interval = 480; // 125 BPM = 480ms
float magThreshold = 1.3;
float magThresholdAvg = magThreshold;
int animation_duration = 60000/120*16;
double beatTime = 60.0 / 140 * 1000;
//
// END BPM STUFF

static void runMicFilter(uint16_t numSamples, float *sampleBuffer) {          // pre-filtering of raw samples (band-pass)

    // low frequency cutoff parameter - see https://dsp.stackexchange.com/questions/40462/exponential-moving-average-cut-off-frequency
    //
    //constexpr float alpha = 0.04f;   // 150Hz
    //constexpr float alpha = 0.03f;   // 110Hz
    constexpr float alpha = 0.0225f; // 80hz
    //constexpr float alpha = 0.01693f;// 60hz

    // high frequency cutoff  parameter
    //
    //constexpr float beta1 = 0.75f;   // 11Khz
    //constexpr float beta1 = 0.82f;   // 15Khz
    //constexpr float beta1 = 0.8285f; // 18Khz
    constexpr float beta1 = 0.85f;  // 20Khz

    constexpr float beta2 = (1.0f - beta1) / 2.0;

    static float last_vals[2] = { 0.0f }; // FIR high freq cutoff filter
    static float lowfilt = 0.0f;          // IIR low frequency cutoff filter

    for (int i=0; i < numSamples; i++) {

        // FIR lowpass, to remove high frequency noise
        //
        float highFilteredSample;
        
        if (i < (numSamples-1)) {
            
            highFilteredSample = beta1*sampleBuffer[i] + beta2*last_vals[0] + beta2*sampleBuffer[i+1];  // smooth out spikes

        } else { 
            
            highFilteredSample = beta1*sampleBuffer[i] + beta2*last_vals[0]  + beta2*last_vals[1];  // spcial handling for last sample in array
        
        }
        
        last_vals[1] = last_vals[0];
        last_vals[0] = sampleBuffer[i];
        sampleBuffer[i] = highFilteredSample;

        // IIR highpass, to remove low frequency noise
        //
        lowfilt += alpha * (sampleBuffer[i] - lowfilt);

        sampleBuffer[i] = sampleBuffer[i] - lowfilt;

    }

}

I2S_datatype postProcessSample(I2S_datatype sample_in) {

    static I2S_datatype lastADCsample = 0;          // last good sample
    static unsigned int broken_samples_counter = 0; // number of consecutive broken (and fixed) ADC samples
    I2S_datatype sample_out = 0;

    // bring sample down down to 16bit unsigned
    I2S_unsigned_datatype rawData = * reinterpret_cast<I2S_unsigned_datatype *> (&sample_in); // C++ acrobatics to get sample as "unsigned"

    #ifndef I2S_USE_16BIT_SAMPLES

        rawData = (rawData >> 16) & 0xFFFF;                       // scale input down from 32bit -> 16bit
        I2S_datatype lastGoodSample = lastADCsample / 16384 ;     // prepare "last good sample" accordingly (26bit-> 12bit with correct sign handling)

    #else

        rawData = rawData & 0xFFFF;                               // input is already in 16bit, just mask off possible junk
        I2S_datatype lastGoodSample = lastADCsample * 4;          // prepare "last good sample" accordingly (10bit-> 12bit)

    #endif

    // decode ADC sample data fields
    //
    uint16_t the_channel = (rawData >> 12) & 0x000F;           // upper 4 bit = ADC channel
    uint16_t the_sample  =  rawData & 0x0FFF;                  // lower 12bit -> ADC sample (unsigned)
    I2S_datatype finalSample = (int(the_sample) - 2048);       // convert unsigned sample to signed (centered at 0);

    if ((the_channel != _myADCchannel) && (_myADCchannel != 0x0F)) { // 0x0F means "don't know what my channel is" 

        // fix bad sample
        //
        finalSample = lastGoodSample;                             // replace with last good ADC sample
        broken_samples_counter ++;
        if (broken_samples_counter > 256) _myADCchannel = 0x0F;   // too  many bad samples in a row -> disable sample corrections
        //Serial.print("\n!ADC rogue sample 0x"); Serial.print(rawData, HEX); Serial.print("\tchannel:");Serial.println(the_channel);

    } else {
        
        broken_samples_counter = 0;                          // good sample - reset counter

    }

    // back to original resolution
    #ifndef I2S_USE_16BIT_SAMPLES
        
        finalSample = finalSample << 16;                          // scale up from 16bit -> 32bit;
    
    #endif

    finalSample = finalSample / 4;                              // mimic old analog driver behaviour (12bit -> 10bit)
    sample_out = (3 * finalSample + lastADCsample) / 4;         // apply low-pass filter (2-tap FIR)
    //sample_out = (finalSample + lastADCsample) / 2;             // apply stronger low-pass filter (2-tap FIR)

    lastADCsample = sample_out;                                 // update ADC last sample
    
    return(sample_out);

}

static void postProcessFFTResults(bool noiseGateOpen, int numberOfChannels) { // post-processing and post-amp of GEQ channels

    for (int i=0; i < numberOfChannels; i++) {

        if (noiseGateOpen) { // noise gate open

            // Adjustment for frequency curves.
            //
            fftCalc[i] *= fftResultPink[i];

            if (FFTScalingMode > 0) {
                
                fftCalc[i] *= FFT_DOWNSCALE;  // adjustment related to FFT windowing function

            }

            // Manual linear adjustment of gain using sampleGain adjustment for different input types.
            //
            fftCalc[i] *= soundAgc ? multAgc : ((float)sampleGain/40.0f * (float)inputLevel/128.0f + 1.0f/16.0f); //apply gain, with inputLevel adjustment
            
            if(fftCalc[i] < 0) {
                
                fftCalc[i] = 0;
                
            }

        }

        // smooth results - rise fast, fall slower
        //
        if(fftCalc[i] > fftAvg[i]) {  // rise fast 
        
            fftAvg[i] = fftCalc[i] *0.75f + 0.25f*fftAvg[i];  // will need approx 2 cycles (50ms) for converging against fftCalc[i]
        
        } else {  // fall slow
        
            if (decayTime < 1000) {
                
                fftAvg[i] = fftCalc[i]*0.22f + 0.78f*fftAvg[i];  // approx  5 cycles (225ms) for falling to zero

            } else if (decayTime < 2000) {
                
                fftAvg[i] = fftCalc[i]*0.17f + 0.83f*fftAvg[i];  // default - approx  9 cycles (225ms) for falling to zero

            } else if (decayTime < 3000) {
                
                fftAvg[i] = fftCalc[i]*0.14f + 0.86f*fftAvg[i];  // approx 14 cycles (350ms) for falling to zero

            } else {
                
                fftAvg[i] = fftCalc[i]*0.1f  + 0.9f*fftAvg[i];  // approx 20 cycles (500ms) for falling to zero
            
            }

        }

        // constrain internal vars - just to be sure
        //
        fftCalc[i] = constrain(fftCalc[i], 0.0f, 1023.0f);
        fftAvg[i] = constrain(fftAvg[i], 0.0f, 1023.0f);

        float currentResult;

        if (limiterOn == true) {

            currentResult = fftAvg[i];
        
        } else {

            currentResult = fftCalc[i];

        }

        switch (FFTScalingMode) {

            case 1: // Logarithmic scaling
                currentResult *= 0.42;                      // 42 is the answer ;-)
                currentResult -= 8.0;                       // this skips the lowest row, giving some room for peaks

                if (currentResult > 1.0) {
                    
                    currentResult = logf(currentResult); // log to base "e", which is the fastest log() function
                
                } else {
                    
                    currentResult = 0.0;                   // special handling, because log(1) = 0; log(0) = undefined
                
                }

                currentResult *= 0.85f + (float(i)/18.0f);  // extra up-scaling for high frequencies
                currentResult = mapf(currentResult, 0, LOG_256, 0, 255); // map [log(1) ... log(255)] to [0 ... 255]
            break;

            case 2: // Linear scaling
                currentResult *= 0.30f;                     // needs a bit more damping, get stay below 255
                currentResult -= 4.0;                       // giving a bit more room for peaks
                
                if (currentResult < 1.0f) {
                    
                    currentResult = 0.0f;

                }

                currentResult *= 0.85f + (float(i)/1.8f);   // extra up-scaling for high frequencies
            break;

            case 3: // square root scaling
                currentResult *= 0.38f;
                currentResult -= 6.0f;

                if (currentResult > 1.0) {
                    
                    currentResult = sqrtf(currentResult);

                } else { 
                    
                    currentResult = 0.0;                   // special handling, because sqrt(0) = undefined
                
                }

                currentResult *= 0.85f + (float(i)/4.5f);   // extra up-scaling for high frequencies
                currentResult = mapf(currentResult, 0.0, 16.0, 0.0, 255.0); // map [sqrt(1) ... sqrt(256)] to [0 ... 255]
            break;

            case 0:
            default: // no scaling - leave freq bins as-is
                currentResult -= 4; // just a bit more room for peaks
            break;

        }

        // Now, let's dump it all into fftResult. Need to do this, otherwise other routines might grab fftResult values prematurely.
        //
        if (soundAgc > 0) {  // apply extra "GEQ Gain" if set by user

            float post_gain = (float)inputLevel/128.0f;

            if (post_gain < 1.0f) {
                
                post_gain = ((post_gain -1.0f) * 0.8f) +1.0f;
            
            }
            
            currentResult *= post_gain;
        
        }
        
        // fftResult[i] = constrain((int)currentResult, 0, 255); // this seems to end up with lots slammed to 255
        fftResult[i] = map(currentResult,0,1023,0,255);
    
    }

}

void getSample() { // post-processing and filtering of MIC sample (micDataReal) from FFTcode()

    float    sampleAdj;           // Gain adjusted sample value
    float    tmpSample;           // An interim sample variable used for calculatioins.
    const float weighting = 0.2f; // Exponential filter weighting. Will be adjustable in a future release.
    const int   AGC_preset = (soundAgc > 0)? (soundAgc-1): 0; // make sure the _compiler_ knows this value will not change while we are inside the function

    #ifdef WLED_DISABLE_SOUND

        micIn = inoise8(millis(), millis());          // Simulated analog read
        micDataReal = micIn;

    #else

        #ifdef ARDUINO_ARCH_ESP32
            
            micIn = int(micDataReal);      // micDataSm = ((micData * 3) + micData)/4;
        
        #else
            
            // this is the minimal code for reading analog mic input on 8266.
            // warning!! Absolutely experimental code. Audio on 8266 is still not working. Expects a million follow-on problems. 
            //            
            static unsigned long lastAnalogTime = 0;
            static float lastAnalogValue = 0.0f;

            if (millis() - lastAnalogTime > 20) {

                micDataReal = analogRead(A0); // read one sample with 10bit resolution. This is a dirty hack, supporting volumereactive effects only.
                lastAnalogTime = millis();
                lastAnalogValue = micDataReal;

                yield();

            } else {
                
                micDataReal = lastAnalogValue;
            }
            
            micIn = int(micDataReal);

        #endif

    #endif

    micLev += (micDataReal-micLev) / 12288.0f;

    if(micIn < micLev) {
        
        micLev = ((micLev * 31.0f) + micDataReal) / 32.0f; // align MicLev to lowest input signal
        
    }

    micIn -= micLev;                                  // Let's center it to 0 now

    // Using an exponential filter to smooth out the signal. We'll add controls for this in a future release.
    //
    float micInNoDC = fabsf(micDataReal - micLev);
    expAdjF = (weighting * micInNoDC + (1.0f-weighting) * expAdjF);
    expAdjF = fabsf(expAdjF);                         // Now (!) take the absolute value

    expAdjF = (expAdjF <= soundSquelch) ? 0: expAdjF; // simple noise gate
    
    if ((soundSquelch == 0) && (expAdjF < 0.25f)) {
        
        expAdjF = 0; // do something meaningfull when "squelch = 0"

    }

    tmpSample = expAdjF;
    micIn = abs(micIn);                               // And get the absolute value of each sample

    sampleAdj = tmpSample * sampleGain / 40.0f * inputLevel/128.0f + tmpSample / 16.0f; // Adjust the gain. with inputLevel adjustment
    sampleReal = tmpSample;

    sampleAdj = fmax(fmin(sampleAdj, 255), 0);        // Question: why are we limiting the value to 8 bits ???
    sampleRaw = (int16_t)sampleAdj;                   // ONLY update sample ONCE!!!!

    // keep "peak" sample, but decay value if current sample is below peak
    //
    if ((sampleMax < sampleReal) && (sampleReal > 0.5f)) {

        sampleMax = sampleMax + 0.5f * (sampleReal - sampleMax);  // new peak - with some filtering

        // another simple way to detect samplePeak
        //
        if ((binNum < 10) && (millis() - timeOfPeak > 80) && (sampleAvg > 1)) {

            samplePeak    = true;
            timeOfPeak    = millis();
            udpSamplePeak = true;

        }

    } else {
    
        if ((multAgc*sampleMax > agcZoneStop[AGC_preset]) && (soundAgc > 0)) {

            sampleMax += 0.5f * (sampleReal - sampleMax);        // over AGC Zone - get back quickly

        } else {

            sampleMax *= agcSampleDecay[AGC_preset];             // signal to zero --> 5-8sec

        }

    }

    if (sampleMax < 0.5f) {
        
        sampleMax = 0.0f;

    }

    sampleAvg = ((sampleAvg * 15.0f) + sampleAdj) / 16.0f;   // Smooth it out over the last 16 samples.
    sampleAvg = fabsf(sampleAvg); // make sure we have a positive value

} // getSample()

void agcAvg(unsigned long the_time) {

    const int AGC_preset = (soundAgc > 0)? (soundAgc-1): 0; // make sure the _compiler_ knows this value will not change while we are inside the function

    float lastMultAgc = multAgc;      // last muliplier used
    float multAgcTemp = multAgc;      // new multiplier
    float tmpAgc = sampleReal * multAgc;        // what-if amplified signal

    float control_error;                        // "control error" input for PI control

    if (last_soundAgc != soundAgc) {
    
        control_integrated = 0.0;                // new preset - reset integrator
    
    }

    // For PI controller, we need to have a constant "frequency"
    // so let's make sure that the control loop is not running at insane speed
    //
    static unsigned long last_time = 0;
    unsigned long time_now = millis();
    
    if ((the_time > 0) && (the_time < time_now)) {
        
        time_now = the_time;  // allow caller to override my clock

    }

    if (time_now - last_time > 2)  {

        last_time = time_now;

        if((fabsf(sampleReal) < 2.0f) || (sampleMax < 1.0f)) {

            // MIC signal is "squelched" - deliver silence
            //
            tmpAgc = 0;
            
            // we need to "spin down" the intgrated error buffer
            //
            if (fabs(control_integrated) < 0.01)  {
                
                control_integrated  = 0.0;
            
            } else {
                
                control_integrated *= 0.91;
                
            }                                  

        } else {

            // compute new setpoint

            if (tmpAgc <= agcTarget0Up[AGC_preset]) {

                multAgcTemp = agcTarget0[AGC_preset] / sampleMax;   // Make the multiplier so that sampleMax * multiplier = first setpoint

            } else {

                multAgcTemp = agcTarget1[AGC_preset] / sampleMax;   // Make the multiplier so that sampleMax * multiplier = second setpoint

            }

        }

        // limit amplification

        if (multAgcTemp > 32.0f) {
            
            multAgcTemp = 32.0f;

        }

        if (multAgcTemp < 1.0f/64.0f) {
            
            multAgcTemp = 1.0f/64.0f;

        }

        // compute error terms

        control_error = multAgcTemp - lastMultAgc;

        if (((multAgcTemp > 0.085f) && (multAgcTemp < 6.5f)) && (multAgc*sampleMax < agcZoneStop[AGC_preset])) {
            
            //integrator ceiling (>140% of max)
        
            control_integrated += control_error * 0.002 * 0.25;   // 2ms = intgration time; 0.25 for damping
        
        } else {

            control_integrated *= 0.9;                            // spin down that beasty integrator

        }

        // apply PI Control 

        tmpAgc = sampleReal * lastMultAgc;                      // check "zone" of the signal using previous gain
        
        if ((tmpAgc > agcZoneHigh[AGC_preset]) || (tmpAgc < soundSquelch + agcZoneLow[AGC_preset])) {  // upper/lower emergy zone

            multAgcTemp = lastMultAgc + agcFollowFast[AGC_preset] * agcControlKp[AGC_preset] * control_error;
            multAgcTemp += agcFollowFast[AGC_preset] * agcControlKi[AGC_preset] * control_integrated;
        
        } else {                                                                         // "normal zone"
            
            multAgcTemp = lastMultAgc + agcFollowSlow[AGC_preset] * agcControlKp[AGC_preset] * control_error;
            multAgcTemp += agcFollowSlow[AGC_preset] * agcControlKi[AGC_preset] * control_integrated;
        
        }

        // limit amplification again - PI controler sometimes "overshoots"
        // multAgcTemp = constrain(multAgcTemp, 0.015625f, 32.0f); // 1/64 < multAgcTemp < 32

        if (multAgcTemp > 32.0f) {
            
            multAgcTemp = 32.0f;
        }
        
        if (multAgcTemp < 1.0f/64.0f) {
            
            multAgcTemp = 1.0f/64.0f;

        }

    }

    // NOW finally amplify the signal

    tmpAgc = sampleReal * multAgcTemp;                  // apply gain to signal

    if (fabsf(sampleReal) < 2.0f) {
        
        tmpAgc = 0.0f;        // apply squelch threshold
    
    }

    //tmpAgc = constrain(tmpAgc, 0, 255);
    
    if (tmpAgc > 255) {
        
        tmpAgc = 255.0f;                  // limit to 8bit
    
    }
    
    if (tmpAgc < 1) {
        
        tmpAgc = 0.0f;                    // just to be sure

    }

    // update global vars ONCE - multAgc, sampleAGC, rawSampleAgc

    multAgc = multAgcTemp;
    rawSampleAgc = 0.8f * tmpAgc + 0.2f * (float)rawSampleAgc;
    
    // update smoothed AGC sample
    
    if (fabsf(tmpAgc) < 1.0f) {

        sampleAgc =  0.5f * tmpAgc + 0.5f * sampleAgc;    // fast path to zero
    
    } else {

        sampleAgc += agcSampleSmooth[AGC_preset] * (tmpAgc - sampleAgc); // smooth path
    
    }

    sampleAgc = fabsf(sampleAgc);                                      // // make sure we have a positive value
    last_soundAgc = soundAgc;

} // agcAvg()

/* 

Limits the dynamics of volumeSmth (= sampleAvg or sampleAgc). 
does not affect FFTResult[] or volumeRaw ( = sample or rawSampleAgc) 

effects: Gravimeter, Gravcenter, Gravcentric, Noisefire, Plasmoid, Freqpixels, Freqwave, Gravfreq, (2D Swirl, 2D Waverly)

*/

void limitSampleDynamics(void) {

    const float bigChange = 196;                  // just a representative number - a large, expected sample value
    static unsigned long last_time = 0;
    static float last_volumeSmth = 0.0f;

    if (limiterOn == false) {
        
        return;

    }

    long delta_time = millis() - last_time;
    delta_time = constrain(delta_time , 1, 1000); // below 1ms -> 1ms; above 1sec -> sily lil hick-up
    float deltaSample = volumeSmth - last_volumeSmth;

    if (attackTime > 0) {                         // user has defined attack time > 0

        float maxAttack =   bigChange * float(delta_time) / float(attackTime);

        if (deltaSample > maxAttack) {
            
            deltaSample = maxAttack;

        }

    }

    if (decayTime > 0) {                          // user has defined decay time > 0

        float maxDecay  = - bigChange * float(delta_time) / float(decayTime);

        if (deltaSample < maxDecay) {
            
            deltaSample = maxDecay;

        }

    }

    volumeSmth = last_volumeSmth + deltaSample; 

    last_volumeSmth = volumeSmth;
    last_time = millis();

}

static void detectSamplePeak(void) {

    bool havePeak = false;

    // Poor man's beat detection by seeing if sample > Average + some value.
    // This goes through ALL of the 255 bins - but ignores stupid settings
    // Then we got a peak, else we don't. The peak has to time out on its own in order to support UDP sound sync.
    
    if ((sampleAvg > 1) && (maxVol > 0) && (binNum > 1) && (vReal[binNum] > maxVol) && ((millis() - timeOfPeak) > 100)) {
    
        havePeak = true;
    
    }

    if (havePeak) {
    
        samplePeak    = true;
        timeOfPeak    = millis();
        udpSamplePeak = true;
    
    }

}

static void autoResetPeak(void) {
  
    // uint16_t MinShowDelay = MAX(50, strip.getMinShowDelay());  // Fixes private class variable compiler error. Unsure if this is the correct way of fixing the root problem. -THATDONFC
    uint16_t MinShowDelay = 50; // Fixes private class variable compiler error. Unsure if this is the correct way of fixing the root problem. -THATDONFC

    if (millis() - timeOfPeak > MinShowDelay) {          // Auto-reset of samplePeak after a complete frame has passed.
        
        samplePeak = false;
        
        if (audioSyncEnabled == 0) {
            
            udpSamplePeak = false;  // this is normally reset by transmitAudioData
        
        }

    }

}

float AD_postProcessFFTResults(float mysample, int bin16map) { // post-processing and post-amp of GEQ channels

    if (FFTScalingMode > 0) {
        
        mysample *= FFT_DOWNSCALE;  // adjustment related to FFT windowing function

    }

    // Manual linear adjustment of gain using sampleGain adjustment for different input types.
    //
    mysample *= soundAgc ? multAgc : ((float)sampleGain/40.0f * (float)inputLevel/128.0f + 1.0f/16.0f); //apply gain, with inputLevel adjustment
    
    if (mysample < 0) {
        
        mysample = 0.0f;
        
    }

    // constrain internal vars - just to be sure
    //
    float currentResult = constrain(mysample, 0.0f, 1023.0f);

    // "i" would normally be 0..15
    // we're just doing this to avoid reverse engineering
    // what was done with the existing idea of 16 bins.
    //
    int i = bin16map;

    switch (FFTScalingMode) {

        case 1: // Logarithmic scaling
            currentResult *= 0.42;                      // 42 is the answer ;-)
            currentResult -= 8.0;                       // this skips the lowest row, giving some room for peaks

            if (currentResult > 1.0) {
                
                currentResult = logf(currentResult); // log to base "e", which is the fastest log() function
            
            } else {
                
                currentResult = 0.0;                   // special handling, because log(1) = 0; log(0) = undefined
            
            }

            currentResult *= 0.85f + (float(i)/18.0f);  // extra up-scaling for high frequencies
            currentResult = mapf(currentResult, 0, LOG_256, 0, 255); // map [log(1) ... log(255)] to [0 ... 255]
        break;

        case 2: // Linear scaling
            currentResult *= 0.30f;                     // needs a bit more damping, get stay below 255
            currentResult -= 4.0;                       // giving a bit more room for peaks
            
            if (currentResult < 1.0f) {
                
                currentResult = 0.0f;

            }

            currentResult *= 0.85f + (float(i)/1.8f);   // extra up-scaling for high frequencies
        break;

        case 3: // square root scaling
            currentResult *= 0.38f;
            currentResult -= 6.0f;

            if (currentResult > 1.0) {
                
                currentResult = sqrtf(currentResult);

            } else { 
                
                currentResult = 0.0;                   // special handling, because sqrt(0) = undefined
            
            }

            currentResult *= 0.85f + (float(i)/4.5f);   // extra up-scaling for high frequencies
            currentResult = mapf(currentResult, 0.0, 16.0, 0.0, 255.0); // map [sqrt(1) ... sqrt(256)] to [0 ... 255]
        break;

        case 0:
        default: // no scaling - leave freq bins as-is
            currentResult -= 4; // just a bit more room for peaks
        break;

    }

    if (soundAgc > 0) {  // apply extra "GEQ Gain" if set by user

        float post_gain = (float)inputLevel/128.0f;

        if (post_gain < 1.0f) {
            
            post_gain = ((post_gain -1.0f) * 0.8f) +1.0f;
        
        }
        
        currentResult *= post_gain;
    
    }
    
    currentResult = constrain((int)currentResult, 0, 255);

    return currentResult;
    
}

void automatic_binner(int steps, byte binarray[], int binstart, int binend) {

    float freqstart = binstart * (SAMPLE_RATE / samplesFFT);
    float freqend = binend * (SAMPLE_RATE / samplesFFT);

    if (freqstart < 1) {

        freqstart = 1.0f; // stop from dividing by zero

    }

    float freqstep = pow((freqend / freqstart),(1.0f/steps));

    float my_freqstart = freqstart;
    
    for (int i=0;i<steps;i++) {

        float my_freqend = my_freqstart * freqstep;

        int my_binstart = round(my_freqstart/(SAMPLE_RATE/samplesFFT));
        int my_binend   = round(my_freqend/(SAMPLE_RATE/samplesFFT));

        // using my own version of postProcessFFTResults that I made non-specifc to 16 bins
        // but without faffing too much with the code, we give it a rough idea of which of
        // the 16 bins it would be on so it can do extra calculations that were hard coded
        // to expect being run in a loop of 0..15

        int bin16map = map(my_binstart,binstart,binend,0,15);

        binarray[i] = AD_postProcessFFTResults(fftAddAvg(my_binstart, my_binend),bin16map);

        my_freqstart = my_freqend;

    }

}

// FFT main code - goes into its own task on its own core
//
void FFTcode( void * pvParameters) {

    for(;;) {

        delay(1);   // DO NOT DELETE THIS LINE! It is needed to give the IDLE(0) task enough time and to keep the watchdog happy.
                    // taskYIELD(), yield(), vTaskDelay() and esp_task_wdt_feed() didn't seem to work.

        uint32_t audio_time = millis();
        static unsigned long lastUMRun = millis();

        unsigned long t_now = millis();      // remember current time
        int userloopDelay = int(t_now - lastUMRun);
        
        if (lastUMRun == 0) {
            
            userloopDelay=0; // startup - don't have valid data from last run.

        }

        // run filters, and repeat in case of loop delays (hick-up compensation)
        //
        if (userloopDelay <2) {
            
            userloopDelay = 0;      // minor glitch, no problem
        
        }

        if (userloopDelay >200) {
            
            userloopDelay = 200;  // limit number of filter re-runs  
        
        }
        
        do {

            getSample();                        // run microphone sampling filters
            agcAvg(t_now - userloopDelay);      // Calculated the PI adjusted value as sampleAvg
            userloopDelay -= 2;                 // advance "simulated time" by 2ms

        } while (userloopDelay > 0);
        
        lastUMRun = t_now;                    // update time keeping

        // update samples for effects (raw, smooth) 
        //
        volumeSmth = (soundAgc) ? sampleAgc   : sampleAvg;
        volumeRaw  = (soundAgc) ? rawSampleAgc: sampleRaw;

        // update FFTMagnitude, taking into account AGC amplification
        //
        my_magnitude = FFT_Magnitude; // / 16.0f, 8.0f, 4.0f done in effects

        if (soundAgc) {
            
            my_magnitude *= multAgc;
        
        }
        
        if (volumeSmth < 1 ) {
            
            my_magnitude = 0.001f;  // noise gate closed - mute

        }

        limitSampleDynamics();

        autoResetPeak();          // auto-reset sample peak after strip minShowDelay

        if (!udpSyncConnected) {
            
            udpSamplePeak = false;  // reset UDP samplePeak while UDP is unconnected

        }

        size_t bytes_read = 0;        /* Counter variable to check if we actually got enough data */
        I2S_datatype newSamples[samples]; /* Intermediary sample storage */

        i2s_read(I2S_PORT, (void *)newSamples, sizeof(newSamples), &bytes_read, portMAX_DELAY);

        if (bytes_read != sizeof(newSamples)) {

            Serial.println("We didn't get the right amount of samples!");

            return;

        }

        for (int i = 0; i < samples; i++) {

            newSamples[i] = postProcessSample(newSamples[i]);

            float currSample = 0.0f;

            #ifdef I2S_SAMPLE_DOWNSCALE_TO_16BIT

                currSample = (float) newSamples[i] / 65536.0f;      // 32bit input -> 16bit; keeping lower 16bits as decimal places
                
            #else

                currSample = (float) newSamples[i];                 // 16bit input -> use as-is

            #endif

            vReal[i] = currSample;

        }

        // band pass filter - can reduce noise floor by a factor of 50
        // downside: frequencies below 100Hz will be ignored
        //
        if (useBandPassFilter) runMicFilter(samplesFFT, vReal);

        // find highest sample in the batch
        //
        float maxSample = 0.0f;                         // max sample from FFT batch

        for (int i=0; i < samplesFFT; i++) {

            // set imaginary parts to 0
            
            vImag[i] = 0;
            
            // pick our current mic sample - we take the max value from all samples that go into FFT
            
            if ((vReal[i] <= (INT16_MAX - 1024)) && (vReal[i] >= (INT16_MIN + 1024))) {  //skip extreme values - normally these are artefacts
            
                if (fabsf((float)vReal[i]) > maxSample) {
                    
                    maxSample = fabsf((float)vReal[i]);

                }

            }

        }

        // release highest sample to volume reactive effects early - not strictly necessary here - could also be done at the end of the function
        // early release allows the filters (getSample() and agcAvg()) to work with fresh values - we will have matching gain and noise gate values when we want to process the FFT results.
        //
        micDataReal = maxSample;

        if (sampleAvg > 0.25f) { 

            #ifdef UM_AUDIOREACTIVE_USE_NEW_FFT

                FFT.dcRemoval();                                            // remove DC offset
                FFT.windowing( FFTWindow::Flat_top, FFTDirection::Forward); // Weigh data using "Flat Top" function - better amplitude accuracy
                //FFT.windowing(FFTWindow::Blackman_Harris, FFTDirection::Forward);  // Weigh data using "Blackman- Harris" window - sharp peaks due to excellent sideband rejection
                FFT.compute( FFTDirection::Forward );                       // Compute FFT
                FFT.complexToMagnitude();                                   // Compute magnitudes

            #else

                FFT.DCRemoval(); // let FFT lib remove DC component, so we don't need to care about this in getSamples()
                //FFT.Windowing( FFT_WIN_TYP_HAMMING, FFT_FORWARD );        // Weigh data - standard Hamming window
                //FFT.Windowing( FFT_WIN_TYP_BLACKMAN, FFT_FORWARD );       // Blackman window - better side freq rejection
                //FFT.Windowing( FFT_WIN_TYP_BLACKMAN_HARRIS, FFT_FORWARD );// Blackman-Harris - excellent sideband rejection
                FFT.Windowing( FFT_WIN_TYP_FLT_TOP, FFT_FORWARD );          // Flat Top Window - better amplitude accuracy
                FFT.Compute( FFT_FORWARD );                             // Compute FFT
                FFT.ComplexToMagnitude();                               // Compute magnitudes

            #endif

            #ifdef UM_AUDIOREACTIVE_USE_NEW_FFT

                FFT.majorPeak(FFT_MajorPeak, FFT_Magnitude);                // let the effects know which freq was most dominant

            #else

                FFT.MajorPeak(&FFT_MajorPeak, &FFT_Magnitude);              // let the effects know which freq was most dominant

            #endif
            
            FFT_MajorPeak = constrain(FFT_MajorPeak, 1.0f, 11025.0f);   // restrict value to range expected by effects
            
        } else { // noise gate closed - only clear results as FFT was skipped. MIC samples are still valid when we do this.

            memset(vReal, 0, sizeof(vReal));
            FFT_MajorPeak = 1;
            FFT_Magnitude = 0.001;

        }

        for (int i = 0; i < samplesFFT; i++) {

            float t = fabsf(vReal[i]);                      // just to be sure - values in fft bins should be positive any way
            vReal[i] = t / 16.0f;                           // Reduce magnitude. Want end result to be scaled linear and ~4096 max.
            fftBin[i] = vReal[i]; // troyhacks

        }

        if (fabsf(sampleAvg) > 0.5f) { 
            
            /*
            * This FFT post processing is a DIY endeavour. What we really need is someone with sound engineering expertise to do a 
            * great job here AND most importantly, that the animations look GREAT as a result.
            *
            * Andrew's updated mapping of 256 bins down to the 16 result bins with Sample Freq = 10240, samplesFFT = 512 and some overlap.
            * Based on testing, the lowest/Start frequency is 60 Hz (with bin 3) and a highest/End frequency of 5120 Hz in bin 255.
            * Now, Take the 60Hz and multiply by 1.320367784 to get the next frequency and so on until the end. Then detetermine the bins.
            * End frequency = Start frequency * multiplier ^ 16
            * Multiplier = (End frequency/ Start frequency) ^ 1/16
            * Multiplier = 1.320367784
            */   

            /* new mapping, optimized for 22050 Hz by softhack007 */
            
            if (useBandPassFilter) {

                // skip frequencies below 100hz
                //
                fftCalc[ 0] = 0.8f * fftAddAvg(3,4);
                fftCalc[ 1] = 0.9f * fftAddAvg(4,5);
                fftCalc[ 2] = fftAddAvg(5,6);
                fftCalc[ 3] = fftAddAvg(6,7);

                // don't use the last bins from 206 to 255. 
                //
                fftCalc[15] = fftAddAvg(165,205) * 0.75f;   // 40 7106 - 8828 high             -- with some damping

            } else {

                fftCalc[ 0] = fftAddAvg(1,2);               // 1    43 - 86   sub-bass
                fftCalc[ 1] = fftAddAvg(2,3);               // 1    86 - 129  bass
                fftCalc[ 2] = fftAddAvg(3,5);               // 2   129 - 216  bass
                fftCalc[ 3] = fftAddAvg(5,7);               // 2   216 - 301  bass + midrange

                // don't use the last bins from 216 to 255. They are usually contaminated by aliasing (aka noise) 
                //
                fftCalc[15] = fftAddAvg(165,215) * 0.70f;   // 50 7106 - 9259 high             -- with some damping

            }

            fftCalc[ 4] = fftAddAvg(7,10);                // 3   301 - 430  midrange
            fftCalc[ 5] = fftAddAvg(10,13);               // 3   430 - 560  midrange
            fftCalc[ 6] = fftAddAvg(13,19);               // 5   560 - 818  midrange
            fftCalc[ 7] = fftAddAvg(19,26);               // 7   818 - 1120 midrange -- 1Khz should always be the center !
            fftCalc[ 8] = fftAddAvg(26,33);               // 7  1120 - 1421 midrange
            fftCalc[ 9] = fftAddAvg(33,44);               // 9  1421 - 1895 midrange
            fftCalc[10] = fftAddAvg(44,56);               // 12 1895 - 2412 midrange + high mid
            fftCalc[11] = fftAddAvg(56,70);               // 14 2412 - 3015 high mid
            fftCalc[12] = fftAddAvg(70,86);               // 16 3015 - 3704 high mid
            fftCalc[13] = fftAddAvg(86,104);              // 18 3704 - 4479 high mid
            fftCalc[14] = fftAddAvg(104,165) * 0.88f;     // 61 4479 - 7106 high mid + high  -- with slight damping

        } else {  // noise gate closed - just decay old values

            for (int i=0; i < NUM_GEQ_CHANNELS; i++) {

                fftCalc[i] *= 0.85f;  // decay to zero

                if (fftCalc[i] < 4.0f) {
                    
                    fftCalc[i] = 0.0f;

                }

            }

        }

        // post-processing of frequency channels (pink noise adjustment, AGC, smooting, scaling)
        //
        postProcessFFTResults((fabsf(sampleAvg) > 0.25f)? true : false , NUM_GEQ_CHANNELS);

        // run peak detection
        //
        autoResetPeak();
        detectSamplePeak();

        for (int i=0; i < 16; i++) {

            AD_fftResult[i] = map(fftCalc[i],0,1023,0,255);
        
        }
            
        specDataMinVolume = AD_fftResult[0];
        specDataMaxVolume = 0;

        for (uint8_t i = 0; i < 16; i++) {

            if (AD_fftResult[i] > specDataMaxVolume) {
                
                specDataMaxVolume = AD_fftResult[i];

            }

            if (AD_fftResult[i] < specDataMinVolume) {
                
                specDataMinVolume = AD_fftResult[i];

            }

        }

        if (fabsf(sampleAvg) > 0.5f) { 

            /* 

            This is the "meat" of what feeds the AuroraDrop animations

            autommatic_binner() applies the same logic from how the original
            sixteen FFT result bins were created - minus some of the extra 
            manual adjustments. Really smooths out these results it seems.

            */

            automatic_binner(128,fftData.specData);
            automatic_binner(64,fftData.specData64);
            automatic_binner(32,fftData.specData32);
            automatic_binner(16,fftData.specData16);
            automatic_binner(8,fftData.specData8);

        }

        // BPM inspiration: https://github.com/blaz-r/ESP32-music-beat-sync/blob/main/src/ESP32-music-beat-sync.cpp
        // It's not currently "great" but it figures it out within a two BPM window.

        magAvg = magAvg * 0.99 + AD_fftResult[0] * 0.01;

        if (millis()-lastBeat > beatTime && AD_fftResult[0]/magAvg > magThreshold) {
            
            bpm_interval = millis() - lastBeat;

            lastBeat = millis();

            if (bpm_interval > 426 && bpm_interval < 600) { // between 100 and 140 BPM (in ms) as a filter for out-of-spec detections

                mean_int = mean_int * 0.9 + bpm_interval * 0.1;

                fftData.bpm = 60*1000 / mean_int;

                animation_duration = 60000/fftData.bpm*16;

                magThresholdAvg = magThresholdAvg * 0.9 + (AD_fftResult[0]/magAvg) * 0.1;

                if (option1Diagnostics && 1 == 2) {
                    
                    Serial.print("\tBEAT! Interval: ");
                    Serial.print(bpm_interval);
                    Serial.print("\tBPM: ");
                    Serial.print(fftData.bpm);
                    Serial.print("\tMeanInt: ");
                    Serial.print(mean_int);
                    Serial.print("\tThreshAvg: ");
                    Serial.print(magThresholdAvg);
                    Serial.print("\tCurThresh: ");
                    Serial.print(AD_fftResult[0]/magAvg);
                    Serial.print("\tStaticThresh: ");
                    Serial.print(magThreshold);
                    Serial.println();

                }

            }

        }
        
        fftData.noAudio = false;

    }

}

void setupAudio() {

    // This is all inspired from the WLED AudioReactive usermod
    // but I removed all the "works for everything" logic so it 
    // just inits an INMP441 mic (and similar I2S mics, maybe)

    Serial.print("INMP441 Audio setup: ");
    Serial.println(I2S_MIC_CHANNEL_TEXT);

    esp_err_t err;

    i2s_config_t i2s_config = {
        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_SAMPLE_RESOLUTION,
        .channel_format = I2S_MIC_CHANNEL,
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL2,
        .dma_buf_count = 8,
        .dma_buf_len = BLOCK_SIZE,
        .use_apll = false,
        .bits_per_chan = I2S_data_size
    };

    const i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,      
        .bck_io_num = I2S_SCK,      // SCK
        .ws_io_num = I2S_WS,        // WS
        .data_out_num = I2S_PIN_NO_CHANGE,         // not used (only for speakers)
        .data_in_num = I2S_SD       // SD .... and depending on the underlying ESP32-IDF, LR may be swapped - like v4.4.3
    };

    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

    if (err != ESP_OK) {

        Serial.printf("Failed installing driver: %d\n", err);
        while (true);

    }

    err = i2s_set_pin(I2S_PORT, &pin_config);

    if (err != ESP_OK) {

        Serial.printf("Failed setting pin: %d\n", err);
        while (true);

    }

    err = i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_SAMPLE_RESOLUTION, I2S_CHANNEL_MONO);  // set bit clocks. Also takes care of MCLK routing if needed.

    if (err != ESP_OK) {

        Serial.printf("Failed setting clock: %d\n", err);
        while (true);

    }

    Serial.println("I2S driver installed.");

    delay(1000); //give everything a second to get going.

    // Test to see if we have a working INMP441 I2S microphone or not.

    float mean = 0.0;
    size_t bytesRead = 0;
    int32_t buffer32[samples] = {0};

    i2s_read(I2S_PORT, &buffer32, sizeof(buffer32), &bytesRead, portMAX_DELAY);

    int samplesRead = bytesRead / 4;

    if (samplesRead == samples) {

        Serial.println("Read the exprected "+String(samples)+" samples");

        for (int i=0; i<samplesRead; i++) {

            mean += abs(buffer32[i] >> 16); 

        }

        if (mean != 0.0) {

            Serial.println("...and they weren't all zeros! Working!");

        } else {

            Serial.println("...and they were all zeros.");
            Serial.println("Check the mic pin definitions and L/R pin and potentially switch L/R to VCC from GND or vice-versa.");

        }

    } else {

        Serial.println("No samples read!");

    }
    
    // Define the FFT Task and lock it to core 0
    //
    xTaskCreatePinnedToCore(
        FFTcode,                          // Function to implement the task
        "FFT",                            // Name of the task
        30000,                            // Stack size in words
        NULL,                             // Task input parameter
        1,                                // Priority of the task
        &FFT_Task,                        // Task handle
    0);                               // Core where the task should run

}