/**
 * @file CardputerTuner.ino
 * @brief M5Cardputer Guitar Tuner
 * @description A guitar tuner that uses the microphone to detect pitch and display
 *              the note name (C, D, E, F, G, A, B) with a visual tuning indicator.
 * @version 1.0
 * @date 2025-12-06
 *
 * @Hardwares: M5Cardputer
 * @Platform Version: Arduino M5Stack Board Manager v2.0.7
 * @Dependent Library:
 * M5GFX: https://github.com/m5stack/M5GFX
 * M5Unified: https://github.com/m5stack/M5Unified
 */

#include <M5Cardputer.h>

// Audio sampling configuration
static constexpr const size_t SAMPLE_RATE = 8000;  // 8kHz sample rate (optimal for hardware)
static constexpr const size_t FFT_SIZE = 512;      // 512 FFT for faster processing
static constexpr const size_t SAMPLE_SIZE = FFT_SIZE;

// Audio buffer
static int16_t audio_buffer[SAMPLE_SIZE];
static float fft_real[FFT_SIZE];  // Real part for FFT
static float fft_imag[FFT_SIZE];  // Imaginary part for FFT
static float fft_output[FFT_SIZE];

// Frequency smoothing and noise reduction
static float prev_frequency = 0;
static int stable_count = 0;
static float noise_floor = 0;
static int noise_samples = 0;

// Display state tracking to reduce flicker
static char prev_noteName[4] = "--";
static int prev_octave = 0;
static float prev_cents = 0;
static float prev_display_freq = 0;
static int no_signal_count = 0;
static bool display_initialized = false;

// Waveform display buffer
static int16_t waveform_buffer[SAMPLE_SIZE];
static bool waveform_updated = false;

// Reference frequency (A4)
static float reference_freq = 440.0f;  // Hz, standard A4
static const float FREQ_STEP = 1.0f;   // 1 Hz step for reference

// Progress bar state tracking to prevent unnecessary redraws
static float prev_display_rms = 0;
static int prev_fillWidth = 0;
static uint16_t prev_barColor = 0;
static int prev_indicatorX = 0;

// Note frequency table (A4 = 440Hz standard tuning)
// Covers guitar range from E2 (82.41Hz) to E6 (1318.51Hz)
const char* NOTE_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const float NOTE_FREQUENCIES[] = {
    // Octave 2
    65.41, 69.30, 73.42, 77.78, 82.41, 87.31, 92.50, 98.00, 103.83, 110.00, 116.54, 123.47,
    // Octave 3
    130.81, 138.59, 146.83, 155.56, 164.81, 174.61, 185.00, 196.00, 207.65, 220.00, 233.08, 246.94,
    // Octave 4
    261.63, 277.18, 293.66, 311.13, 329.63, 349.23, 369.99, 392.00, 415.30, 440.00, 466.16, 493.88,
    // Octave 5
    523.25, 554.37, 587.33, 622.25, 659.25, 698.46, 739.99, 783.99, 830.61, 880.00, 932.33, 987.77
};
const int NUM_NOTES = sizeof(NOTE_FREQUENCIES) / sizeof(NOTE_FREQUENCIES[0]);

// Display dimensions
int DISPLAY_WIDTH;
int DISPLAY_HEIGHT;
int CENTER_X;
int CENTER_Y;

// Bit reversal for FFT
void bitReverse(float* real, float* imag, int n) {
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            // Swap real parts
            float temp = real[i];
            real[i] = real[j];
            real[j] = temp;
            // Swap imaginary parts
            temp = imag[i];
            imag[i] = imag[j];
            imag[j] = temp;
        }
        int k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
}

// Fast Fourier Transform (Cooley-Tukey Radix-2 DIT)
void performFFT(float* real, float* imag, int n) {
    // Bit reversal
    bitReverse(real, imag, n);

    // FFT computation
    for (int size = 2; size <= n; size *= 2) {
        float angle = -2.0 * PI / size;
        float wreal = cos(angle);
        float wimag = sin(angle);

        for (int i = 0; i < n; i += size) {
            float wr = 1.0;
            float wi = 0.0;

            for (int j = 0; j < size / 2; j++) {
                int k = i + j;
                int l = k + size / 2;

                // Butterfly operation
                float treal = wr * real[l] - wi * imag[l];
                float timag = wr * imag[l] + wi * real[l];

                real[l] = real[k] - treal;
                imag[l] = imag[k] - timag;
                real[k] += treal;
                imag[k] += timag;

                // Update twiddle factors
                float temp = wr;
                wr = wr * wreal - wi * wimag;
                wi = temp * wimag + wi * wreal;
            }
        }
    }
}

// Compute magnitude spectrum from FFT result
void computeMagnitude(float* real, float* imag, float* magnitude, int n) {
    for (int i = 0; i < n / 2; i++) {
        magnitude[i] = sqrt(real[i] * real[i] + imag[i] * imag[i]);
    }
}

// Parabolic interpolation for sub-bin frequency accuracy
float parabolicInterpolation(float* bins, int peakIndex, int maxIndex) {
    if (peakIndex <= 0 || peakIndex >= maxIndex - 1) {
        return peakIndex;
    }

    float alpha = bins[peakIndex - 1];
    float beta = bins[peakIndex];
    float gamma = bins[peakIndex + 1];

    float p = 0.5 * (alpha - gamma) / (alpha - 2 * beta + gamma);
    return peakIndex + p;
}

// Find dominant frequency using FFT with improved precision and noise reduction
float findDominantFrequency(int16_t* samples, int numSamples) {
    // Calculate RMS to check signal strength
    float rms = 0;
    for (int i = 0; i < numSamples; i++) {
        rms += samples[i] * samples[i];
    }
    rms = sqrt(rms / numSamples);

    // Adaptive noise floor estimation
    if (noise_samples < 10) {
        noise_floor = (noise_floor * noise_samples + rms) / (noise_samples + 1);
        noise_samples++;
    } else {
        // Gradually adapt to changing noise levels
        if (rms < noise_floor * 2) {
            noise_floor = 0.95 * noise_floor + 0.05 * rms;
        }
    }

        // More conservative threshold for stability
    float minThreshold = (30.0f > noise_floor * 1.5f) ? 30.0f : noise_floor * 1.5f;
    if (rms < minThreshold) {
        return 0;
    }

    // Copy and normalize samples to FFT input
    float maxVal = 1.0;
    for (int i = 0; i < numSamples; i++) {
        if (abs(samples[i]) > maxVal) {
            maxVal = abs(samples[i]);
        }
    }

    // Copy to real array and apply Hann window
    for (int i = 0; i < FFT_SIZE; i++) {
        if (i < numSamples) {
            fft_real[i] = ((float)samples[i] / maxVal) * (0.5 * (1 - cos(2 * PI * i / (FFT_SIZE - 1))));
        } else {
            fft_real[i] = 0;
        }
        fft_imag[i] = 0;  // Initialize imaginary part to 0
    }

    // Perform FFT
    performFFT(fft_real, fft_imag, FFT_SIZE);

    // Compute magnitude spectrum
    computeMagnitude(fft_real, fft_imag, fft_output, FFT_SIZE);

    // Noise subtraction - estimate and subtract noise floor from FFT output
    float noiseLevel = 0;
    int noiseBins = 0;
    int minBin = (int)(60.0 * FFT_SIZE / SAMPLE_RATE);
    int maxBin = (int)(1500.0 * FFT_SIZE / SAMPLE_RATE);
    if (maxBin > FFT_SIZE / 2) maxBin = FFT_SIZE / 2;

    // Calculate noise level from lower magnitude bins
    for (int i = minBin; i < maxBin; i++) {
        if (fft_output[i] < 20) {
            noiseLevel += fft_output[i];
            noiseBins++;
        }
    }
    if (noiseBins > 0) {
        noiseLevel /= noiseBins;
        // Subtract noise from all bins
        for (int i = minBin; i < maxBin; i++) {
            fft_output[i] = max(0.0f, fft_output[i] - noiseLevel);
        }
    }

    // Find peak frequency in guitar range
    int peakIndex = minBin;
    float peakValue = fft_output[minBin];

    for (int i = minBin; i < maxBin; i++) {
        if (fft_output[i] > peakValue) {
            peakValue = fft_output[i];
            peakIndex = i;
        }
    }

    // Higher peak threshold for stability
    if (peakValue < 6.0) {
        return 0;
    }

    // Use parabolic interpolation for sub-bin accuracy
    float refinedIndex = parabolicInterpolation(fft_output, peakIndex, FFT_SIZE / 2);

    // Convert bin to frequency with interpolation
    float frequency = refinedIndex * SAMPLE_RATE / FFT_SIZE;

    // Very strong smoothing for maximum stability
    if (prev_frequency > 0 && abs(frequency - prev_frequency) < 12) {
        // Extra heavy smoothing - maximize stability
        float smoothFactor = (abs(frequency - prev_frequency) < 1) ? 0.90 : 0.80;
        frequency = smoothFactor * frequency + (1 - smoothFactor) * prev_frequency;
        stable_count++;
    } else {
        stable_count = 0;
    }
    prev_frequency = frequency;

    // Require significant stability before accepting
    if (stable_count < 6) {
        return prev_frequency > 0 ? prev_frequency : 0;
    }

    return frequency;
}

// Find closest note to a given frequency
void findClosestNote(float frequency, char* noteName, int* octave, float* cents) {
    if (frequency < 20 || frequency > 2000) {
        strcpy(noteName, "--");
        *octave = 0;
        *cents = 0;
        return;
    }

    float minDiff = 10000;
    int closestIndex = 0;

    for (int i = 0; i < NUM_NOTES; i++) {
        // Scale frequency based on reference A4
        float adjustedTargetFreq = NOTE_FREQUENCIES[i] * (reference_freq / 440.0f);
        float diff = abs(frequency - adjustedTargetFreq);
        if (diff < minDiff) {
            minDiff = diff;
            closestIndex = i;
        }
    }

    // Calculate octave and note
    *octave = (closestIndex / 12) + 2;
    int noteIndex = closestIndex % 12;
    strcpy(noteName, NOTE_NAMES[noteIndex]);

    // Calculate cents (100 cents = 1 semitone)
    float targetFreq = NOTE_FREQUENCIES[closestIndex] * (reference_freq / 440.0f);
    *cents = 1200 * log2(frequency / targetFreq);
}

// Draw tuning progress bar (with smart dirty region detection)
void drawTuningBar(const char* noteName, float cents, float rms, bool fullRedraw) {
    // Progress bar display area
    int barY = 95;
    int barHeight = 35;
    int barWidth = DISPLAY_WIDTH - 20;
    int barX = 10;

    // Safety check
    if (noteName == nullptr || barWidth <= 0) {
        return;
    }

    // If no signal, show empty bar (once)
    if (strcmp(noteName, "--") == 0 || !waveform_updated) {
        if (!fullRedraw) return;  // Don't redraw empty bar repeatedly
        M5Cardputer.Display.fillRect(barX, barY, barWidth, barHeight, TFT_BLACK);
        M5Cardputer.Display.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
        int centerX = barX + barWidth / 2;
        M5Cardputer.Display.drawFastVLine(centerX, barY, barHeight, TFT_DARKGREY);
        return;
    }

    // Smooth RMS to prevent jitter
    if (prev_display_rms == 0) {
        prev_display_rms = rms;
    } else {
        prev_display_rms = 0.85 * prev_display_rms + 0.15 * rms;
    }

    // Normalize RMS to 0-1 range
    float normalizedRMS = prev_display_rms / 500.0f;
    normalizedRMS = constrain(normalizedRMS, 0.0f, 1.0f);

    // Determine bar color based on tuning accuracy
    uint16_t barColor;
    if (abs(cents) < 5) {
        barColor = TFT_GREEN;
    } else if (abs(cents) < 15) {
        barColor = TFT_YELLOW;
    } else if (abs(cents) < 30) {
        barColor = TFT_ORANGE;
    } else {
        barColor = TFT_RED;
    }

    // Calculate fill width and indicator position
    int fillWidth = (int)(normalizedRMS * barWidth * 0.8);
    fillWidth = constrain(fillWidth, 10, barWidth - 10);

    float clampedCents = constrain(cents, -50, 50);
    float position = 0.5 + (clampedCents / 100.0);
    position = constrain(position, 0.0f, 1.0f);
    int indicatorX = barX + (int)(position * barWidth);

    // Smart dirty region detection - only redraw if changed significantly
    bool needsRedraw = fullRedraw;
    if (!fullRedraw) {
        // Check if any visual element changed significantly
        if (abs(fillWidth - prev_fillWidth) > 3 ||        // Width changed >3px
            barColor != prev_barColor ||                   // Color changed
            abs(indicatorX - prev_indicatorX) > 2) {      // Indicator moved >2px
            needsRedraw = true;
        }
    }

    // Skip redraw if nothing changed
    if (!needsRedraw) {
        return;
    }

    // Store current state
    prev_fillWidth = fillWidth;
    prev_barColor = barColor;
    prev_indicatorX = indicatorX;

    // Clear and redraw
    if (fullRedraw) {
        M5Cardputer.Display.fillRect(barX, barY, barWidth, barHeight, TFT_BLACK);
        M5Cardputer.Display.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
    } else {
        M5Cardputer.Display.fillRect(barX + 1, barY + 1, barWidth - 2, barHeight - 2, TFT_BLACK);
    }

    // Draw center line
    int centerX = barX + barWidth / 2;
    M5Cardputer.Display.drawFastVLine(centerX, barY, barHeight, TFT_DARKGREY);

    // Draw tick marks
    for (int i = -3; i <= 3; i++) {
        if (i == 0) continue;
        int tickX = centerX + (i * barWidth / 8);
        M5Cardputer.Display.drawFastVLine(tickX, barY + barHeight - 5, 5, TFT_DARKGREY);
    }

    // Draw filled bar from left to right
    int fillHeight = barHeight - 4;
    int fillY = barY + 2;
    int barStartX = barX + 2;

    M5Cardputer.Display.fillRect(barStartX, fillY, fillWidth, fillHeight, barColor);
    M5Cardputer.Display.drawFastHLine(barStartX, fillY, fillWidth, TFT_WHITE);

    // Draw indicator line
    M5Cardputer.Display.drawFastVLine(indicatorX, barY + 1, barHeight - 2, TFT_WHITE);
    M5Cardputer.Display.drawFastVLine(indicatorX - 1, barY + 1, barHeight - 2, TFT_WHITE);
    M5Cardputer.Display.drawFastVLine(indicatorX + 1, barY + 1, barHeight - 2, TFT_WHITE);
}

// Draw the tuner display with reduced flicker
void drawTuner(const char* noteName, int octave, float cents, float frequency, float rms) {
    // Check if we have no signal
    bool hasSignal = (strcmp(noteName, "--") != 0);

    // Track no signal state
    if (!hasSignal) {
        no_signal_count++;
    } else {
        no_signal_count = 0;
    }

    // Only update display if values changed significantly
    bool shouldUpdate = false;

    if (!hasSignal && no_signal_count == 1) {
        // Just lost signal
        shouldUpdate = true;
    } else if (hasSignal && strcmp(prev_noteName, "--") == 0) {
        // Just got signal
        shouldUpdate = true;
    } else if (hasSignal) {
        // Check if note or frequency changed significantly - extremely conservative
        if (strcmp(noteName, prev_noteName) != 0 ||
            octave != prev_octave) {
            shouldUpdate = true;
        }
        // Only update for very large frequency changes
        else if (abs(cents - prev_cents) > 8.0 ||
                 abs(frequency - prev_display_freq) > 5.0) {
            shouldUpdate = true;
        }
    }

    // Only update text area when needed, selectively update progress bar
    if (!shouldUpdate && no_signal_count < 2) {
        // Update progress bar only when there's signal
        if (hasSignal) {
            drawTuningBar(noteName, cents, rms, false);
        }
        return;
    }

    // Store current values
    strcpy(prev_noteName, noteName);
    prev_octave = octave;
    prev_cents = cents;
    prev_display_freq = frequency;

    // On first display or when switching between signal states, clear once
    if (!display_initialized) {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        display_initialized = true;
        // Also reset progress bar tracking on initialization
        prev_fillWidth = 0;
        prev_barColor = 0;
        prev_indicatorX = 0;
        prev_display_rms = 0;
    } else {
        // Only clear specific text areas to minimize flicker
        // Clear note name area
        M5Cardputer.Display.fillRect(0, 10, DISPLAY_WIDTH, 50, TFT_BLACK);
        // Clear frequency area
        M5Cardputer.Display.fillRect(0, 50, DISPLAY_WIDTH, 40, TFT_BLACK);
    }

    // Draw note name and octave (larger and centered)
    M5Cardputer.Display.setTextSize(5);
    if (strcmp(noteName, "--") == 0) {
        M5Cardputer.Display.setTextColor(TFT_DARKGREY);
        M5Cardputer.Display.drawString("--", CENTER_X, 30);
    } else {
        M5Cardputer.Display.setTextColor(TFT_WHITE);
        String noteStr = String(noteName) + String(octave);
        M5Cardputer.Display.drawString(noteStr, CENTER_X, 30);
    }

    // Draw reference frequency (A4) - always visible
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(TFT_CYAN);
    M5Cardputer.Display.drawString("A4=" + String(reference_freq, 1) + "Hz", CENTER_X, 55);

    // Draw target frequency for current note
    if (strcmp(noteName, "--") != 0) {
        M5Cardputer.Display.setTextColor(TFT_DARKGREY);
        float targetFreq = NOTE_FREQUENCIES[(octave - 2) * 12 + (strchr(NOTE_NAMES[0], noteName[0]) - NOTE_NAMES[0])];
        for (int i = 0; i < NUM_NOTES; i++) {
            if (strcmp(noteName, NOTE_NAMES[i % 12]) == 0 && octave == (i / 12) + 2) {
                targetFreq = NOTE_FREQUENCIES[i];
                break;
            }
        }
        targetFreq *= (reference_freq / 440.0f);
        M5Cardputer.Display.drawString("Target: " + String(targetFreq, 1) + " Hz", CENTER_X, 63);
    }

    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(TFT_YELLOW);
    if (frequency > 0) {
        M5Cardputer.Display.drawString(String(frequency, 1) + " Hz", CENTER_X, 78);
    } else {
        M5Cardputer.Display.drawString("--- Hz", CENTER_X, 78);
    }

    // Draw tuning progress bar (status is shown by bar color, no text needed)
    drawTuningBar(noteName, cents, rms, true);
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    // Initialize display
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.fillScreen(TFT_BLACK);

    DISPLAY_WIDTH = M5Cardputer.Display.width();
    DISPLAY_HEIGHT = M5Cardputer.Display.height();
    CENTER_X = DISPLAY_WIDTH / 2;
    CENTER_Y = DISPLAY_HEIGHT / 2;

    // Turn off speaker (microphone and speaker cannot be used simultaneously)
    M5Cardputer.Speaker.end();

    // Initialize microphone
    M5Cardputer.Mic.begin();

    // Show startup screen
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(TFT_CYAN);
    M5Cardputer.Display.drawString("Guitar Tuner", CENTER_X, CENTER_Y - 20);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(TFT_WHITE);
    M5Cardputer.Display.drawString("Initializing...", CENTER_X, CENTER_Y + 10);
    delay(1000);

    M5Cardputer.Display.fillScreen(TFT_BLACK);
}

void loop() {
    M5Cardputer.update();

    // Check for key presses to adjust frequency offset
    if (M5Cardputer.Keyboard.isChange()) {
        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

            // Check for arrow keys (using KEY modifier)
            for (auto i : status.word) {
                if (i == ';') {  // Arrow up: increase reference frequency
                    reference_freq += FREQ_STEP;
                    if (reference_freq > 450.0f) reference_freq = 450.0f;  // Limit to 450Hz
                    // Force display update
                    prev_noteName[0] = '\0';
                    display_initialized = false;
                }
                else if (i == '.') {  // Arrow down: decrease reference frequency
                    reference_freq -= FREQ_STEP;
                    if (reference_freq < 430.0f) reference_freq = 430.0f;  // Limit to 430Hz
                    // Force display update
                    prev_noteName[0] = '\0';
                    display_initialized = false;
                }
            }
        }
    }

    // Record audio samples
    if (M5Cardputer.Mic.isEnabled()) {
        if (M5Cardputer.Mic.record(audio_buffer, SAMPLE_SIZE, SAMPLE_RATE)) {
            // Copy waveform data for display
            memcpy(waveform_buffer, audio_buffer, sizeof(audio_buffer));
            waveform_updated = true;

            // Calculate RMS once for the entire loop
            float rms = 0;
            for (int i = 0; i < SAMPLE_SIZE; i++) {
                rms += (float)waveform_buffer[i] * waveform_buffer[i];
            }
            rms = sqrt(rms / SAMPLE_SIZE);

            // Find dominant frequency
            float frequency = findDominantFrequency(audio_buffer, SAMPLE_SIZE);

            // Find closest note
            char noteName[4] = "--";  // Initialize to prevent undefined behavior
            int octave = 0;
            float cents = 0;
            findClosestNote(frequency, noteName, &octave, &cents);

            // Draw tuner display
            drawTuner(noteName, octave, cents, frequency, rms);
        }
    }

    delay(25);  // Balanced refresh rate for guitar tuning
}
