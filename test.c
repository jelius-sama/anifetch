#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define NUM_RUNS 10
#define SINGLE_THREADED_PATH "/usr/local/bin/anime"
#define MULTI_THREADED_PATH "./anifetch"

typedef struct {
    double times[NUM_RUNS];
    double total;
    double average;
    double min;
    double max;
    double median;
} BenchmarkResult;

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

int compare_doubles(const void *a, const void *b) {
    double diff = *(double*)a - *(double*)b;
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

void calculate_stats(BenchmarkResult *result) {
    result->total = 0.0;
    result->min = result->times[0];
    result->max = result->times[0];
    
    for (int i = 0; i < NUM_RUNS; i++) {
        result->total += result->times[i];
        if (result->times[i] < result->min) result->min = result->times[i];
        if (result->times[i] > result->max) result->max = result->times[i];
    }
    
    result->average = result->total / NUM_RUNS;
    
    // Calculate median
    double sorted[NUM_RUNS];
    memcpy(sorted, result->times, sizeof(result->times));
    qsort(sorted, NUM_RUNS, sizeof(double), compare_doubles);
    
    if (NUM_RUNS % 2 == 0) {
        result->median = (sorted[NUM_RUNS/2 - 1] + sorted[NUM_RUNS/2]) / 2.0;
    } else {
        result->median = sorted[NUM_RUNS/2];
    }
}

int run_benchmark(const char *program_path, BenchmarkResult *result, const char *args) {
    printf("Benchmarking: %s\n", program_path);
    printf("Running %d iterations...\n", NUM_RUNS);
    
    // Check if program exists
    if (access(program_path, X_OK) != 0) {
        fprintf(stderr, "Error: Cannot execute %s\n", program_path);
        return 0;
    }
    
    char command[512];
    
    for (int i = 0; i < NUM_RUNS; i++) {
        // Build command with output redirection to avoid cluttering terminal
        snprintf(command, sizeof(command), "%s %s > /dev/null 2>&1", program_path, args ? args : "");
        
        double start = get_time_ms();
        int ret = system(command);
        double end = get_time_ms();
        
        if (ret != 0) {
            fprintf(stderr, "Warning: Run %d returned non-zero exit code: %d\n", i + 1, ret);
        }
        
        result->times[i] = end - start;
        printf("  Run %d: %.2f ms\n", i + 1, result->times[i]);
        
        // Small delay between runs to let system stabilize
        usleep(100000); // 100ms
    }
    
    calculate_stats(result);
    printf("Completed!\n\n");
    
    return 1;
}

void print_report(BenchmarkResult *single, BenchmarkResult *multi) {
    printf("\n");
    printf("================================================================================\n");
    printf("                    ANIFETCH PERFORMANCE BENCHMARK REPORT\n");
    printf("================================================================================\n");
    printf("\n");
    printf("Number of runs: %d\n", NUM_RUNS);
    printf("\n");
    
    printf("--------------------------------------------------------------------------------\n");
    printf("Single-threaded version (%s)\n", SINGLE_THREADED_PATH);
    printf("--------------------------------------------------------------------------------\n");
    printf("  Average:  %.2f ms\n", single->average);
    printf("  Median:   %.2f ms\n", single->median);
    printf("  Min:      %.2f ms\n", single->min);
    printf("  Max:      %.2f ms\n", single->max);
    printf("  Total:    %.2f ms\n", single->total);
    printf("\n");
    
    printf("  Individual runs:\n");
    for (int i = 0; i < NUM_RUNS; i++) {
        printf("    Run %2d: %.2f ms\n", i + 1, single->times[i]);
    }
    printf("\n");
    
    printf("--------------------------------------------------------------------------------\n");
    printf("Multi-threaded version (%s)\n", MULTI_THREADED_PATH);
    printf("--------------------------------------------------------------------------------\n");
    printf("  Average:  %.2f ms\n", multi->average);
    printf("  Median:   %.2f ms\n", multi->median);
    printf("  Min:      %.2f ms\n", multi->min);
    printf("  Max:      %.2f ms\n", multi->max);
    printf("  Total:    %.2f ms\n", multi->total);
    printf("\n");
    
    printf("  Individual runs:\n");
    for (int i = 0; i < NUM_RUNS; i++) {
        printf("    Run %2d: %.2f ms\n", i + 1, multi->times[i]);
    }
    printf("\n");
    
    printf("--------------------------------------------------------------------------------\n");
    printf("PERFORMANCE COMPARISON\n");
    printf("--------------------------------------------------------------------------------\n");
    
    double speedup = single->average / multi->average;
    double improvement = ((single->average - multi->average) / single->average) * 100.0;
    
    printf("  Average speedup:     %.2fx\n", speedup);
    printf("  Average improvement: %.2f%%\n", improvement);
    printf("\n");
    
    if (speedup > 1.05) {
        printf("  Result: Multi-threaded version is FASTER ✓\n");
    } else if (speedup < 0.95) {
        printf("  Result: Multi-threaded version is SLOWER ✗\n");
    } else {
        printf("  Result: Performance is SIMILAR (~)\n");
    }
    printf("\n");
    
    printf("  Median speedup:      %.2fx\n", single->median / multi->median);
    printf("  Min time comparison: %.2f ms vs %.2f ms (%.2fx)\n", 
           single->min, multi->min, single->min / multi->min);
    printf("  Max time comparison: %.2f ms vs %.2f ms (%.2fx)\n", 
           single->max, multi->max, single->max / multi->max);
    printf("\n");
    
    printf("================================================================================\n");
    printf("                              END OF REPORT\n");
    printf("================================================================================\n");
}

int main(int argc, char *argv[]) {
    printf("================================================================================\n");
    printf("           ANIFETCH BENCHMARK TEST - Single vs Multi-threaded\n");
    printf("================================================================================\n");
    printf("\n");
    
    // Get command line arguments to pass to both programs
    char args[256] = "";
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            strcat(args, argv[i]);
            if (i < argc - 1) strcat(args, " ");
        }
        printf("Using arguments: %s\n\n", args);
    } else {
        printf("Note: No image path provided. Programs will use default from config.\n\n");
    }
    
    BenchmarkResult single_result;
    BenchmarkResult multi_result;
    
    // Run single-threaded benchmark
    printf("Phase 1: Testing single-threaded version\n");
    printf("--------------------------------------------------------------------------------\n");
    if (!run_benchmark(SINGLE_THREADED_PATH, &single_result, args)) {
        fprintf(stderr, "Failed to benchmark single-threaded version\n");
        return 1;
    }
    
    printf("\n");
    
    // Run multi-threaded benchmark
    printf("Phase 2: Testing multi-threaded version\n");
    printf("--------------------------------------------------------------------------------\n");
    if (!run_benchmark(MULTI_THREADED_PATH, &multi_result, args)) {
        fprintf(stderr, "Failed to benchmark multi-threaded version\n");
        return 1;
    }
    
    // Print comprehensive report
    print_report(&single_result, &multi_result);
    
    return 0;
}
