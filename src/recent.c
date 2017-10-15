/*
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pcre.h>

#ifdef DEBUG
#define printd(...) fprintf(stderr, __VA_ARGS__)
#else
#define printd(...) //
#endif

struct {
    pcre *regex;
    pcre_extra *re_extra;
    char *format;
    int has_tz;
} TS_FORMAT = { .format = NULL, .regex = NULL, .re_extra = NULL, .has_tz = 0 };

static long int local_gmtoff;

off_t find_newline(char *file, off_t pos, off_t min, off_t max, int direction)
{
    printd("find_newline, min %lu, max %lu, direction %i\n", min, max, direction);
    for(;;) {
        if (file[pos] == '\n') return pos;
        if (pos == min || pos == max) return -1;
        pos += (direction > 0?1:-1);
    }
}

#ifdef DEBUG
void print_line(char *file, off_t pos)
{
    // off_t end = find_newline(file, pos, 0, 1000, 1);
    write(STDERR_FILENO, &file[pos], 80);
    printd("...\n");
}
#endif

void get_ts_format(char *descr)
{
    FILE *f;
    f = fopen("timestamps.conf", "r");
    if (f == NULL) {
        f = fopen("/etc/recent/timestamps.conf", "r");
        if (f == NULL) {
            perror("Cannot read timestamps.conf!");
            exit(-1);
        }
    }

    ssize_t len; size_t bufsize = 512; 
    char *buf = (char *)malloc(bufsize);
    char *fmt;
    for (;;) {
        len = getline(&buf, &bufsize, f);
        if (len == -1) {
            break;
        }
        if (buf[0] == '#') {
            continue;
        }
        if (buf[len-1] == '\n') {
            buf[len-1] = '\0';
        }

        fmt = strsep(&buf, "=");
        if (!strcmp(fmt, descr)) {
            char *re_ptr = strsep(&buf,"|");
            printd("regex '%s', fmt '%s'\n", re_ptr, buf);
            if (buf == NULL) {
                printf("Error parsing timestamp format: '|' missing\n");
                exit(3);
            }
            if (strlen(re_ptr) > 0) { 
                const char *pcre_error;
                int pcre_erroffset;
                TS_FORMAT.regex = pcre_compile(re_ptr, PCRE_UTF8, &pcre_error, &pcre_erroffset, NULL);    
                if (TS_FORMAT.regex == NULL) {
                    printf("PCRE compilation failed at offset %d: %s\n", pcre_erroffset, pcre_error);
                    exit(3);
                }
                TS_FORMAT.re_extra = pcre_study(TS_FORMAT.regex, 0, &pcre_error);
                if (pcre_error != NULL) {
                    printf("Errors studying pattern: %s\n", pcre_error);
                    exit(3);
                }
            } else {
                TS_FORMAT.regex = NULL;
            }
            TS_FORMAT.format = buf;
            if (strstr(TS_FORMAT.format, "%z") != NULL || strstr(TS_FORMAT.format, "%Z") != NULL) {
                printd("Taking timezone into account");
                TS_FORMAT.has_tz = 1;
            } else {
                printd("Timezone missing, treat all timestamps as local time");
                TS_FORMAT.has_tz = 0;
            }
            return;
        }
        buf = fmt;
    }
    printf("Unknown timestamp format!\n");
    exit(3);
}

time_t get_nearest_timestamp(char *file, off_t *pos, off_t min, off_t max, int direction)
{
    printd("get_nearest_timestamp pos %lu, min %lu, max %lu, dir %i\n", *pos, min, max, direction);
    struct tm ts;
    int rc;
    int ovector[30];
    char t[100];
    char *rest = NULL;
    memset(&ts, 0, sizeof(ts));
    for(;;) {
        printd("Searching nearest ts, direction %i\n", direction);
        if (TS_FORMAT.regex != NULL) {
            rc = pcre_exec(TS_FORMAT.regex, TS_FORMAT.re_extra, &file[*pos], max - *pos, 0, 0, ovector, 30);
            if (rc > 0) {
                pcre_copy_substring(&file[*pos], ovector, rc, 1, t, 100);
                printd("Matching against '%s'\n", t);
                rest = strptime(&file[*pos + ovector[2]], TS_FORMAT.format, &ts);
                if (TS_FORMAT.has_tz) {
                    // if timestamp format contains timezone, convert ts to local time as far as mktime expects local time
                    // otherwise, do nothing, i.e. treat all timestamps in file given in local time
                    ts.tm_sec = ts.tm_sec - ts.tm_gmtoff + local_gmtoff;
                    ts.tm_gmtoff = local_gmtoff;
                }
                strftime(t, 100, "%F %T %z", &ts);
                printd("match! ts: %s\n", t);
            } else {
                rest = NULL;
            }
        } else {
            rest = strptime(&file[*pos], TS_FORMAT.format, &ts);
        }

        if (rest == NULL) {
            *pos = find_newline(file, *pos, min, max, (direction > 0?1:-1)) + 1;
            if (*pos == 0) {
                break;
            }
        } else {
            if (ts.tm_year == 0) {
                /* some moronic timestamp formats are missing year */
                time_t now = time(NULL);
                ts.tm_year = localtime(&now)->tm_year;
            }
            return mktime(&ts);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int c;
    char *filename;
    char *file;
    time_t seconds = 0;
    int fd;
    off_t filesize;
    time_t target_timestamp;

    while((c = getopt(argc, argv, "hn:t:")) != -1) {
        switch(c) {
            case 'n':
                seconds = atol(optarg);
                if (errno == EINVAL || seconds <= 0) {
                    printf("Specify positive integer value for '-n'\n");
                    exit(-1);
                }
                break;
            case 't':
                get_ts_format(optarg);
                break;
            case 'h':
                printf("USAGE:\t%s [-n SECONDS] [-t FORMAT] FILE\n", argv[0]);
                exit(0);
                break;
        }
    }

    if (seconds == 0) {
        printf("Specify positive integer value for '-n'\n");
        exit(-1);
    }
    if (TS_FORMAT.format == NULL) {
        printf("Specify timestamp format with '-t'\n");
        exit(-1);
    }


    if (optind  > argc - 1) {
        printf("Specify file to process!\n");
        exit(-1);
    } else {
        filename = argv[optind];
    }

    struct stat st;
    stat(filename, &st);
    filesize = st.st_size;
    if (!S_ISREG(st.st_mode)) {
        printf("%s should be a regular file!\n", filename);
        exit(-1);
    }

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Cannot open file");
        exit(1);
    }
    file = mmap(NULL, filesize, PROT_READ, MAP_SHARED, fd, 0);
    if (file == MAP_FAILED) {
        perror("Cannot mmap file");
        exit(2);
    }

    off_t chunk_start = 0;
    off_t chunk_end = st.st_size;
    off_t pivot;
    off_t prev_ts_pos;
    off_t next_ts_pos;
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    local_gmtoff = tm_now->tm_gmtoff;
    target_timestamp = now - seconds;
    unsigned iterations = 0;

    for (;;) {
        iterations++;
        pivot = (chunk_start + chunk_end)/2;
        prev_ts_pos = next_ts_pos = pivot;
        /*
        time_t next_ts = get_nearest_timestamp(file, &next_ts_pos, chunk_start, chunk_end, 1);
        time_t prev_ts = get_nearest_timestamp(file, &prev_ts_pos, chunk_start, chunk_end, -1); 
        */

        char n[100], p[100], t[100];
        time_t next_ts = get_nearest_timestamp(file, &next_ts_pos, chunk_start, st.st_size, 1);
        time_t prev_ts = get_nearest_timestamp(file, &prev_ts_pos, 0, chunk_end, -1);

        ctime_r(&next_ts, n); 
        ctime_r(&prev_ts, p);
        ctime_r(&target_timestamp, t);
        //printd("next %sprev %starget %s\n", n, p, t);
        printd(">> next %lu prev %lu target %lu\n", next_ts, prev_ts, target_timestamp);

        if (next_ts == 0 && prev_ts < target_timestamp) {
            /* nothing found */
            printd("Nothing found\n");
            break;
        }
        if (prev_ts == 0 && next_ts >= target_timestamp ) {
            /* whole file is matching target ts */
            write(STDOUT_FILENO, &file[prev_ts_pos], filesize);
            break;
        }
        if (target_timestamp == prev_ts) {
            write(STDOUT_FILENO, &file[prev_ts_pos], filesize - prev_ts_pos);
            break;
        } else if (next_ts >= target_timestamp && prev_ts < target_timestamp) {
            write(STDOUT_FILENO, &file[next_ts_pos], filesize - next_ts_pos);
            break;
        }
        if (target_timestamp > next_ts) {
            chunk_start = pivot;
        } else {
            chunk_end = pivot;
        }
    }

    printd(" ====> done in %u iterations\n", iterations);

    munmap(file, 0);
    close(fd);
    exit(0);
}
