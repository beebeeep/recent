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

char *java_fmt = "%Y-%m-%d %H:%M:%S";
char *syslog_fmt = "%b  %d %H:%M:%S";

off_t find_newline(char *file, off_t pos, off_t min, off_t max, int direction)
{
    for(;;) {
        if (file[pos] == '\n') return pos;
        if (pos == min || pos == max) return -1;
        pos += (direction > 0?1:-1);
    }
}

void print_line(char *file, off_t pos)
{
    off_t end = find_newline(file, pos, 0, 1000, 1);
    write(STDOUT_FILENO, &file[pos], end - pos);
}

time_t get_nearest_timestamp(char *fmt, char *file, off_t *pos, off_t min, off_t max, int direction)
{
    struct tm ts;
    char *rest = NULL;
    memset(&ts, 0, sizeof(ts));
    for(;;) {
        rest = strptime(&file[*pos], fmt, &ts);
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
    char *ts_format;
    time_t seconds;
    int fd;
    off_t filesize;
    time_t target_timestamp;

    while((c = getopt(argc, argv, "n:t:")) != -1) {
        switch(c) {
            case 'n':
                seconds = atol(optarg);
                if (errno == EINVAL) {
                    printf("Specify integer value for '-n'\n");
                    exit(-1);
                }
                break;
            case 't':
                if (!strcmp(optarg, "java")) {
                    ts_format = java_fmt;
                } else if (!strcmp(optarg, "syslog")) {
                    ts_format = syslog_fmt;
                } else {
                    printf("Unknown timestamp format!\n");
                    exit(3);
                }
                break;
        }
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
    target_timestamp = time(NULL) - seconds;

    for (;;) {
        pivot = (chunk_start + chunk_end)/2;
        prev_ts_pos = next_ts_pos = pivot;
        time_t next_ts = get_nearest_timestamp(ts_format, file, &next_ts_pos, chunk_start, chunk_end, 1);
        time_t prev_ts = get_nearest_timestamp(ts_format, file, &prev_ts_pos, chunk_start, chunk_end, -1);
//        printf("next %sprev %s", ctime(&next_ts), ctime(&prev_ts));
        if (next_ts == 0 && prev_ts < target_timestamp) {
            /* nothing found */
            break;
        }
//        if (prev_ts == 0 && next_ts )
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

    munmap(file, 0);
    close(fd);
    return 0;
}
