/* SPDX-License-Identifier: GPL-2.0 */
static const char *__doc__ = "XDP stats program\n"
	" - Finding xdp_stats_map via --dev name info\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <locale.h>
#include <unistd.h>
#include <time.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <net/if.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */

#include "../common/common_params.h"
#include "../common/common_user_bpf_xdp.h"
#include "common_kern_user.h"

static const struct option long_options[] = {
	{"help",        no_argument,		NULL, 'h' },
	{"dev",         required_argument,	NULL, 'd' },
	{"quiet",       no_argument,		NULL, 'q' },
	{0, 0, NULL,  0 }
};

static __u32 get_map_fd_type(int map_fd)
{
	struct bpf_map_info info = {};
	__u32 info_len = sizeof(info);
	int err;

	if (map_fd < 0)
		return BPF_MAP_TYPE_UNSPEC;

        /* BPF-info via bpf-syscall */
	err = bpf_obj_get_info_by_fd(map_fd, &info, &info_len);
	if (err) {
		fprintf(stderr, "ERR: %s() can't get info - %s\n",
			__func__,  strerror(errno));
		exit(EXIT_FAIL_BPF) ;
	}
	if (verbose)
		printf(" - BPF map (bpf_map_type:%d) id:%d name:%s\n",
		       info.type, info.id, info.name);

	return info.type;
}

int find_map_fd(struct bpf_object *bpf_obj, const char *mapname)
{
	int map_fd;

	map_fd = bpf_object__find_map_fd_by_name(bpf_obj, mapname);
        if (map_fd < 0) {
		fprintf(stderr, "ERR: cannot find map by name: %s\n", mapname);
	}
	return map_fd;
}

#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
static __u64 gettime(void)
{
	struct timespec t;
	int res;

	res = clock_gettime(CLOCK_MONOTONIC, &t);
	if (res < 0) {
		fprintf(stderr, "Error with gettimeofday! (%i)\n", res);
		exit(EXIT_FAIL);
	}
	return (__u64) t.tv_sec * NANOSEC_PER_SEC + t.tv_nsec;
}

struct record {
	__u64 timestamp;
	struct datarec total; /* defined in common_kern_user.h */
};

struct stats_record {
	struct record stats;
};

static double calc_period(struct record *r, struct record *p)
{
	double period_ = 0;
	__u64 period = 0;

	period = r->timestamp - p->timestamp;
	if (period > 0)
		period_ = ((double) period / NANOSEC_PER_SEC);

	return period_;
}

static void stats_print(struct stats_record *stats_rec,
			struct stats_record *stats_prev)
{
	struct record *rec, *prev;
	double period;
	__u64 packets;
	double pps;

	/* Assignment#2: Print other XDP actions stats  */
	{
		char *fmt = "%-12s RX-pkts:%'-11lld pps:%'-10.0f period:%f\n";
		char *action = "XDP_PASS";
		rec  = &stats_rec->stats;
		prev = &stats_prev->stats;

		period = calc_period(rec, prev);
		if (period == 0)
		       return;

		packets = rec->total.rx_packets - prev->total.rx_packets;
		pps     = packets / period;

		printf(fmt, action, rec->total.rx_packets, pps, period);
	}
}

void map_get_value_array(int fd, __u32 key, struct datarec *value)
{
	if ((bpf_map_lookup_elem(fd, &key, value)) != 0) {
		fprintf(stderr,
			"ERR: bpf_map_lookup_elem failed key:0x%X\n", key);
	}
}

static bool map_collect(int fd, __u32 map_type, __u32 key, struct record *rec)
{
	struct datarec value;

	/* Get time as close as possible to reading map contents */
	rec->timestamp = gettime();

	switch (map_type) {
	case BPF_MAP_TYPE_ARRAY:
		map_get_value_array(fd, key, &value);
		break;
	case BPF_MAP_TYPE_PERCPU_ARRAY:
		/* fall-through */
	default:
		fprintf(stderr, "ERR: Unknown map_type(%u) cannot handle\n",
			map_type);
		return false;
		break;
	}

	/* Assignment#1: Add byte counters */
	rec->total.rx_packets = value.rx_packets;
	return true;
}

static void stats_collect(int map_fd, __u32 map_type,
			  struct stats_record *stats_rec)
{
	/* Assignment#2: Collect other XDP actions stats  */
	__u32 key = XDP_PASS;

	map_collect(map_fd, map_type, key, &stats_rec->stats);
}

static void stats_poll(int map_fd, int interval)
{
	struct stats_record prev, record = { 0 };
	__u32 map_type;

	/* Trick to pretty printf with thousands separators use %' */
	setlocale(LC_NUMERIC, "en_US");

	if (verbose) {
		printf("\nCollecting stats from BPF map\n");
	}
	map_type = get_map_fd_type(map_fd);

	/* Print stats "header" */
	if (verbose) {
		printf("\n");
		printf("%-12s\n", "XDP-action");
	}

	/* Get initial reading quickly */
	stats_collect(map_fd, map_type, &record);
	usleep(1000000/4);

	while (1) {
		prev = record; /* struct copy */
		stats_collect(map_fd, map_type, &record);
		stats_print(&record, &prev);
		sleep(interval);
	}
}

int open_bpf_map_file(const char *pin_dir, const char *mapname)
{
	char filename[PATH_MAX];
	int len, fd;

	len = snprintf(filename, PATH_MAX, "%s/%s", pin_dir, mapname);
	if (len < 0) {
		fprintf(stderr, "ERR: constructing full mapname path\n");
		return -1;
	}

	/* Lesson: There is only a weak dependency to libbpf here as bpf_obj_get
	 * is a simple wrapper around the bpf-syscall
	 */
	fd = bpf_obj_get(filename);
	if (fd < 0) {
		fprintf(stderr,
			"WARN: Failed to open bpf map file:%s err(%d):%s\n",
			filename, errno, strerror(errno));
		return fd;
	}
	return fd;
}

#ifndef PATH_MAX
#define PATH_MAX	4096
#endif

const char *pin_basedir =  "/sys/fs/bpf";

int main(int argc, char **argv)
{
	struct bpf_object *bpf_obj;
	char pin_dir[PATH_MAX];
	int stats_map_fd;
	int interval = 2;
	int err, len;

	struct config cfg = {
		.ifindex   = -1,
		.do_unload = false,
	};

	/* Cmdline options can change progsec */
	parse_cmdline_args(argc, argv, long_options, &cfg, __doc__);

	/* Required option */
	if (cfg.ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n");
		usage(argv[0], __doc__, long_options);
		return EXIT_FAIL_OPTION;
	}

	/* Use the --dev name as subdir for finding pinned maps */
	len = snprintf(pin_dir, PATH_MAX, "%s/%s", pin_basedir, cfg.ifname);
	if (len < 0) {
		fprintf(stderr, "ERR: creating pin dirname\n");
		return EXIT_FAIL_OPTION;
	}

	stats_map_fd = open_bpf_map_file(pin_dir, "xdp_stats_map");
	if (stats_map_fd < 0) {
		return EXIT_FAIL_BPF;
	}

	stats_poll(stats_map_fd, interval);

	return EXIT_OK;
}
