#include <stdio.h>
#include <stdint.h>

void calculate_heuristics(uint8_t *frame, uint8_t *bg, int len) {
    int coverage = 0;
    long long total_intensity = 0;
    
    for (int i = 0; i < len; i++) {
        int val = frame[i];
        int bval = bg ? bg[i] : 0;
        
        if (val > bval + 2) {
            val -= bval;
        } else {
            val = 0;
        }
        
        if (val > 10) { // arbitrary threshold for a real ridge
            coverage++;
            total_intensity += val;
        }
    }
    
    int percentage = (coverage * 100) / len;
    int avg_intensity = coverage > 0 ? (total_intensity / coverage) : 0;
    
    printf("Coverage: %d%% (%d/%d), Avg Intensity: %d\n", percentage, coverage, len, avg_intensity);
}
