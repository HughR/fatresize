/*
  $Id: fatresize.c,v 1.6 2005/09/09 12:57:39 siome_tajshe Exp $
  Copyright (C) 2005  Anton D. Kachalov <mouse@ya.ru>

  The FAT16/FAT32 non-destructive resizer.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <getopt.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <parted/parted.h>
#include <parted/debug.h>
#include <parted/unit.h>

#include "config.h"

#ifdef LIBPARTED_GT_2_4
#define FAT_ASSERT(cond, action) PED_ASSERT(cond)
#else
#define FAT_ASSERT(cond, action) PED_ASSERT(cond, action)
#endif

#define FAT32MIN	1024*1024*512

static struct {
    unsigned char *dev;
    unsigned char pnum;
    PedSector size;
    char verbose;
    char progress;
    char is_evms;
    char info;
} opts;

typedef struct {
    time_t last_update;
    time_t predicted_time_left;
} TimerContext;

static TimerContext timer_context;

static void
usage(int code)
{
    fprintf(stdout, "Usage: %s [options] device (e.g. /dev/hda1, /dev/sda2)\n"
		    "    Resize an FAT16/FAT32 volume non-destructively:\n\n"
		    "    -s, --size SIZE    Resize volume to SIZE[k|M|G|ki|Mi|Gi] bytes\n"
		    "    -i, --info         Show volume information\n"
		    "    -p, --progress     Show progress\n"
		    "    -q, --quite        Be quite\n"
		    "    -v, --verbose      Verbose\n"
		    "    -h, --help         Display this help\n\n"
		    "Please report bugs to %s\n",
		    PACKAGE_NAME, PACKAGE_BUGREPORT);

    exit(code);
}

static void
printd(int level, const char *fmt, ...)
{
    va_list ap;

    if (opts.verbose < level)
	return;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static PedSector
get_size(char *s)
{
    PedSector size;
    char *suffix;
    int prefix_kind = 1000;

    size = strtoll(s, &suffix, 10);
    if (size <= 0 || errno == ERANGE)
    {
	fprintf(stderr, "Illegal new volume size\n");
	usage(1);
    }

    if (!*suffix)
	return size;

    if (strlen(suffix) == 2 && suffix[1] == 'i')
	prefix_kind = 1024;
    else if (strlen(suffix) > 1)
	usage(1);

    switch (*suffix)
    {
	case 'G':
	    size *= prefix_kind;
	case 'M':
	    size *= prefix_kind;
	case 'k':
	    size *= prefix_kind;
	    break;
	default:
	    usage(1);
    }

    return size;
}

static int
get_partnum(char *dev)
{
    int pnum;
    char *p;

    p = dev+strlen(dev)-1;
    while (*p && isdigit(*p) && *p != '/')
	p--;

    pnum = atoi(p+1);
    return pnum ? pnum : 1;
}

static char *
get_devname(char *dev)
{
    char *devname;
    char *p;

    p = dev+strlen(dev)-1;
    while (*p && isdigit(*p) && *p != '/')
	p--;

    devname = malloc(p-dev+2);
    strncpy(devname, dev, p-dev+1);
    devname[p-dev+1] = '\0';

    if (opts.is_evms)
    {
	p = strstr(devname, "/evms/");
	if (p)
	    strcpy(p, p+5);
    }

    return devname;
}

static void
resize_handler(PedTimer *timer, void *ctx)
{
#if 0
    int draw_this_time;
    TimerContext* tctx = (TimerContext*) ctx;

    if (tctx->last_update != timer->now && timer->now > timer->start)
    {
	tctx->predicted_time_left = timer->predicted_end - timer->now;
	tctx->last_update = timer->now;
	draw_this_time = 1;
    }
    else
	draw_this_time = 0;

    if (draw_this_time)
    {
	printf("\r                                                            \r");
	if (timer->state_name)
	    printf("%s... ", timer->state_name);
	printf("%0.f%%\t(time left %.2ld:%.2ld)",
		100.0 * timer->frac,
		tctx->predicted_time_left / 60,
		tctx->predicted_time_left % 60);

	fflush(stdout);
    }
#endif
    fprintf(stdout, ".");
    fflush(stdout);
}

static PedExceptionOption
fatresize_handler(PedException *ex)
{
    if (opts.verbose != -1)
	fprintf(stderr, "%s: %s\n", ped_exception_get_type_string(ex->type),
				    ex->message);

    if (ex->type >= PED_EXCEPTION_ERROR)
	return PED_EXCEPTION_CANCEL;

    switch (ex->options)
    {
	case PED_EXCEPTION_IGNORE_CANCEL:
	    return PED_EXCEPTION_IGNORE;
	default:
	    return PED_EXCEPTION_UNHANDLED;
    }
}

/* This function changes "sector" to "new_sector" if the new value lies
 * within the required range.
 */
static int
snap(PedSector* sector, PedSector new_sector, PedGeometry* range)
{
    FAT_ASSERT(ped_geometry_test_sector_inside (range, *sector), return 0);
    if (!ped_geometry_test_sector_inside(range, new_sector))
	return 0;

    *sector = new_sector;
    return 1;
}

/* This function tries to replace the value in sector with a sequence
 * of possible replacements, given in order of preference.  The first
 * replacement that lies within the required range is adopted.
 */
static void
try_snap(PedSector* sector, PedGeometry* range, ...)
{
    va_list list;

    va_start(list, range);
    while (1)
    {
	PedSector new_sector = va_arg (list, PedSector);
	if (new_sector == -1)
	    break;
	if (snap(sector, new_sector, range))
	    break;
    }
    va_end(list);
}

/* Snaps a partition to nearby partition boundaries.  This is useful for
 * gobbling up small amounts of free space, and also for reinterpreting small
 * changes to a partition as non-changes (eg: perhaps the user only wanted to
 * resize the end of a partition).
 * 	Note that this isn't the end of the story... this function is
 * always called before the constraint solver kicks in.  So you don't need to
 * worry too much about inadvertantly creating overlapping partitions, etc.
 */
static void
snap_to_boundaries (PedGeometry* new_geom, PedGeometry* old_geom,
		    PedDisk* disk,
		    PedGeometry* start_range, PedGeometry* end_range)
{
	PedPartition*	start_part;
	PedPartition*	end_part;
	PedSector	start = new_geom->start;
	PedSector	end = new_geom->end;

	start_part = ped_disk_get_partition_by_sector (disk, start);
	end_part = ped_disk_get_partition_by_sector (disk, end);

	if (old_geom) {
		try_snap (&start, start_range,
			  old_geom->start, start_part->geom.start,
			  start_part->geom.end + 1, -1);
		try_snap (&end, end_range,
			  old_geom->end, end_part->geom.end,
			  end_part->geom.start - 1, -1);
	} else {
		try_snap (&start, start_range,
			  start_part->geom.start, start_part->geom.end + 1, -1);
		try_snap (&end, end_range,
			  end_part->geom.end, end_part->geom.start - 1, -1);
	}

	FAT_ASSERT (start <= end, return);
	ped_geometry_set (new_geom, start, end - start + 1);
}

/* This functions constructs a constraint from the following information:
 * 	start, is_start_exact, end, is_end_exact.
 * 	
 * If is_start_exact == 1, then the constraint requires start be as given in
 * "start".  Otherwise, the constraint does not set any requirements on the
 * start.
 */
static PedConstraint*
constraint_from_start_end (PedDevice* dev, PedGeometry* range_start,
                           PedGeometry* range_end)
{
    return ped_constraint_new(ped_alignment_any, ped_alignment_any,
			      range_start, range_end, 1, dev->length);
}

static PedConstraint*
constraint_intersect_and_destroy(PedConstraint* a, PedConstraint* b)
{
    PedConstraint* result = ped_constraint_intersect(a, b);
    ped_constraint_destroy(a);
    ped_constraint_destroy(b);
    return result;
}

static int
partition_warn_busy(PedPartition* part)
{
    char* path = ped_partition_get_path(part);

    if (ped_partition_is_busy(part))
    {
	ped_exception_throw(PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
			("Partition %s is being used.  You must unmount it "
			 "before you modify it with Parted."),
			path);
	free(path);
	return 0;
    }

    free(path);
    return 1;
}

int
main(int argc, char **argv)
{
    int opt;

    PedDevice *dev;
    PedDisk *disk;
    PedPartition *part;
    PedTimer *timer = NULL;

    char *old_str;
    char *def_str;
    PedFileSystem *fs;
    PedSector start, end;
    PedGeometry new_geom;
    PedGeometry *old_geom;
    PedConstraint *constraint;
    PedGeometry *range_start, *range_end;

    static const char *sopt = "-his:vpq";
    static const struct option lopt[] = {
	{"help",	no_argument,		NULL, 'h'},
	{"info",	no_argument,		NULL, 'i'},
	{"progress",	no_argument,		NULL, 'p'},
	{"size",	required_argument,	NULL, 's'},
	{"verbose",	no_argument,		NULL, 'v'},
	{"quite",	no_argument,		NULL, 'q'},
	{NULL, 0, NULL, 0}
    };

    memset(&opts, 0, sizeof(opts));

    if (argc < 2)
	usage(0);

    while ((opt = getopt_long(argc, argv, sopt, lopt, NULL)) != -1)
    {
	switch (opt)
	{
	    case 1:
		if (!opts.dev)
		{
		    if (!strncmp(optarg, "/dev/evms/", 10))
			opts.is_evms = 1;
		    opts.dev = get_devname(optarg);
		    opts.pnum = get_partnum(optarg);
		}
		else
		    usage(1);
		break;
	    case 'i':
		opts.info = 1;
		break;
	    case 'p':
		opts.progress = 1;
		break;
	    case 's':
		opts.size = get_size(optarg);
		break;
	    case 'v':
		opts.verbose++;
		break;
	    case 'q':
		opts.verbose = -1;
		break;
	    case 'h':
	    case '?':
	    default:
		printd(0, "%s (%s)\n", PACKAGE_STRING, BUILD_DATE);
		usage(0);
	}
    }

    printd(0, "%s (%s)\n", PACKAGE_STRING, BUILD_DATE);

    if (!opts.dev)
    {
	fprintf(stderr, "You must specify exactly one device.\n");
	return 1;
    }
    else if (!opts.size && !opts.info)
    {
	fprintf(stderr, "You must specify new size.\n");
	return 1;
    }

    ped_exception_set_handler(fatresize_handler);

    if (opts.progress)
    {
	timer = ped_timer_new(resize_handler, &timer_context);
	timer_context.last_update = 0;
    }

    printd(3, "ped_device_get(%s)\n", opts.dev);
    dev = ped_device_get(opts.dev);
    if (!dev)
	return 1;

    printd(3, "ped_device_open()\n");
    if (!ped_device_open(dev))
	return 1;

    printd(3, "ped_disk_new()\n");
    disk = ped_disk_new(dev);
    if (!disk)
	return 1;

    printd(3, "ped_disk_get_partition(%d)\n", opts.pnum);
    part = ped_disk_get_partition(disk, opts.pnum);
    if (!part || !part->fs_type)
	return 1;

    if (strncmp(part->fs_type->name, "fat", 3))
    {
#if 0
	if (opts.is_evms)
	{
	    ped_exception_throw(PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
				"%s is not valid FAT16/FAT32 partition.",
				opts.dev);
	}
	else
#endif
	{
	    ped_exception_throw(PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
				"%s%d is not valid FAT16/FAT32 partition.",
				opts.dev, opts.pnum);
	}
	return 1;
    }

    if (!partition_warn_busy(part))
    {
	ped_disk_destroy(disk);
	return 1;
    }

    if (opts.info)
    {
	printd(3, "ped_file_system_open()\n");
	fs = ped_file_system_open(&part->geom);
	if (!fs)
	    return 1;

	printd(3, "ped_file_system_get_resize_constraint()\n");
	constraint = ped_file_system_get_resize_constraint(fs);
	if (!constraint)
	    return 1;

	printf("FAT: %s\n", part->fs_type->name);
	printf("Size: %llu\n", fs->geom->length * dev->sector_size);
	printf("Min size: %llu\n", (part->fs_type->name[3] == '3'
		&& (constraint->min_size * dev->sector_size) < FAT32MIN ?
			FAT32MIN : constraint->min_size * dev->sector_size));
	printf("Max size: %llu\n", constraint->max_size * dev->sector_size);

	ped_constraint_destroy(constraint);
	return 0;
    }

    start = part->geom.start;
    printd(3, "ped_geometry_new(%llu)\n", start);
    range_start = ped_geometry_new (dev, start, 1);
    if (!range_start)
	return 1;

    end = part->geom.start + opts.size / dev->sector_size;
    printd(3, "ped_unit_parse(%llu)\n", end);
    old_str = ped_unit_format(dev, part->geom.end);
    def_str = ped_unit_format(dev, end);
    if (!strcmp(old_str, def_str))
    {
	range_end = ped_geometry_new(dev, part->geom.end, 1);
	if (!range_end)
	    return 1;
    }
    else if (!ped_unit_parse(def_str, dev, &end, &range_end))
	return 1;
    free(old_str);
    free(def_str);

    printd(3, "ped_geometry_duplicate()\n");
    old_geom = ped_geometry_duplicate(&part->geom);
    if (!old_geom)
	return 1;

    printd(3, "ped_geometry_init(%llu, %llu)\n", start, end - start + 1);
    if (!ped_geometry_init(&new_geom, dev, start, end - start + 1))
	return 1;

    printd(3, "snap_to_boundaries()\n");
    snap_to_boundaries(&new_geom, &part->geom, disk, range_start, range_end);

    printd(3, "ped_file_system_open()\n");
    fs = ped_file_system_open(&part->geom);
    if (!fs)
	return 1;

    printd(3, "constraint_intersect_and_destroy()\n");
    constraint = constraint_intersect_and_destroy(
		    ped_file_system_get_resize_constraint(fs),
		    constraint_from_start_end(dev, range_start, range_end)
		    );
    if (!constraint)
        return 1;

    /* FAT32 must be bigger than 512Mb */
    if (part->fs_type->name[3] == '3' && opts.size < FAT32MIN)
    {
	ped_exception_throw(PED_EXCEPTION_ERROR, PED_EXCEPTION_CANCEL,
			    "FAT32 partition must be bigger than 512Mb.");
	return 1;
    }

    /* Resize partition */
    printd(3, "ped_disk_set_partition_geom(%llu, %llu)\n", new_geom.start, new_geom.end);
    if (!ped_disk_set_partition_geom(disk, part, constraint,
				     new_geom.start, new_geom.end))
    {
	ped_file_system_close(fs);
	ped_constraint_destroy(constraint);
	return 1;
    }

    printd(1, "Resizing file system.\n");
    if (!ped_file_system_resize(fs, &part->geom, timer))
        return 1;

    printd(1, "Done.\n");
    /* May have changed... fat16 -> fat32 */
    ped_partition_set_system(part, fs->type);
    ped_file_system_close(fs);
    ped_constraint_destroy(constraint);

    if (!opts.is_evms)
    {
	printd(1, "Committing changes.\n");
	if (!ped_disk_commit(disk))
	    return 1;
    }
    else
    {
	printd(3, "ped_constraint_exact()\n");
	constraint = ped_constraint_exact(old_geom);
	if (!constraint)
	    return 1;

	printd(3, "ped_disk_set_partition_geom(%llu, %llu)\n", old_geom->start, old_geom->end);
	if (!ped_disk_set_partition_geom(disk, part, constraint,
					 old_geom->start, old_geom->end))
	{
	    ped_constraint_destroy(constraint);
	    return 1;
	}
	ped_constraint_destroy(constraint);

	printd(1, "Commiting changes only to disk.\n");
	if (!ped_disk_commit_to_dev(disk))
	    return 1;
    }
    ped_disk_destroy(disk);

    if (dev->boot_dirty && dev->type != PED_DEVICE_FILE)
    {
	ped_exception_throw(PED_EXCEPTION_WARNING, PED_EXCEPTION_OK,
		 ("You should reinstall your boot loader."
		  "Read section 4 of the Parted User "
		  "documentation for more information."));
    }

    ped_device_close(dev);

    return 0;
}
