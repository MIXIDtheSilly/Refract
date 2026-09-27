// Records from the Android default input for N seconds (default 3) and prints the peak and RMS
// level per half second, to check that the emulator passes the host microphone through.
// Build: tools\mic_probe\build.cmd; run as root (adb root): adb shell /data/local/tmp/mic_probe [seconds] [preset]
#include <aaudio/AAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 3;
    const int preset = argc > 2 ? atoi(argv[2]) : 0;  // AAUDIO_INPUT_PRESET_*, e.g. 7 = VOICE_COMMUNICATION
    AAudioStreamBuilder* builder = NULL;
    AAudioStream* stream = NULL;
    AAudio_createStreamBuilder(&builder);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    if (preset) AAudioStreamBuilder_setInputPreset(builder, preset);
    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        printf("open failed: %s\n", AAudio_convertResultToText(result));
        return 1;
    }
    const int rate = AAudioStream_getSampleRate(stream);
    printf("recording %d s at %d Hz\n", seconds, rate);
    AAudioStream_requestStart(stream);
    const int chunk = rate / 2;
    int16_t* buffer = malloc(sizeof(int16_t) * chunk);
    int overallPeak = 0;
    for (int i = 0; i < seconds * 2; ++i) {
        int got = 0;
        while (got < chunk) {
            int n = AAudioStream_read(stream, buffer + got, chunk - got, 1000000000LL);
            if (n < 0) {
                printf("read failed: %s\n", AAudio_convertResultToText(n));
                return 1;
            }
            got += n;
        }
        int peak = 0;
        double sum = 0;
        for (int s = 0; s < chunk; ++s) {
            int v = abs(buffer[s]);
            if (v > peak) peak = v;
            sum += (double)buffer[s] * buffer[s];
        }
        if (peak > overallPeak) overallPeak = peak;
        printf("%4.1fs peak %5d rms %7.1f\n", (i + 1) * 0.5, peak, sqrt(sum / chunk));
    }
    printf("overall peak %d%s\n", overallPeak, overallPeak == 0 ? " (silence: host audio input is not reaching the guest)" : "");
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    return 0;
}
