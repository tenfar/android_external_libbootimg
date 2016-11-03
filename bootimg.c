/*
** Copyright 2016, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include "bootimg.h"
#include "mincrypt/sha.h"

#define APP_NAME "bootimg"

#ifdef DEBUG
#include <android/log.h>
#define LOGV(...) { __android_log_print(ANDROID_LOG_INFO,  APP_NAME, __VA_ARGS__); printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { __android_log_print(ANDROID_LOG_ERROR, APP_NAME, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define LOGV(...) { printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#endif

// create new files as 0644
#define NEW_FILE_PERMISSIONS (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

// create new directories as 0755
#define NEW_DIR_PERMISSIONS (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

enum {
	MODE_NONE,
	MODE_UNPACK,
	MODE_CREATE,
	MODE_UPDATE,
	MODE_INFO
};

static int write_string_to_file(const char *file, const char *string)
{
	int fd, len = strlen(string);

	fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, NEW_FILE_PERMISSIONS);
	if (fd < 0)
		return errno;

	if (len && write(fd, string, len) != len)
		goto oops;

	if (write(fd, "\n", 1) != 1)
		goto oops;

	close(fd);
	return 0;
oops:
	close(fd);
	return EIO;
}

static int read_string_from_file(const char* file, char *buf, off_t len)
{
	int fd;
	char *c;
	off_t rlen;

	if ((fd = open(file, O_RDONLY)) < 0)
		return errno;

	rlen = read(fd, buf, len - 1);
	if (rlen < 0)
		goto oops;

	*(buf + rlen + 1) = 0;

	if ((c = strchr(buf, '\r')) || (c = strchr(buf, '\n')))
		*c = 0; // stop at newline

	close(fd);
	return 0;
oops:
	close(fd);
	return EIO;
}

static char *basename(char const *path)
{
	const char *s = strrchr(path, '/');
	if (!s) {
		return strdup(path);
	} else {
		return strdup(s + 1);
	}
}

static char *read_hash(const byte *hash)
{
	char *str = malloc(SHA_DIGEST_SIZE * 2 + 1);
	char *c = str;
	const byte *h = hash;
	for (; c < str + SHA_DIGEST_SIZE * 2; h++) {
		c += sprintf(c, "%02x", *h);
	}
	*c = 0;
	return str;
}

/* args set by user in command */
enum
{
	ARG_BOARD           = 1 <<  0,
	ARG_CMDLINE         = 1 <<  1,
	ARG_PAGESIZE        = 1 <<  2,
	ARG_BASE            = 1 <<  3,
	ARG_KERNEL_OFFSET   = 1 <<  4,
	ARG_RAMDISK_OFFSET  = 1 <<  5,
	ARG_SECOND_OFFSET   = 1 <<  6,
	ARG_TAGS_OFFSET     = 1 <<  7,
	ARG_KERNEL          = 1 <<  8,
	ARG_RAMDISK         = 1 <<  9,
	ARG_SECOND          = 1 << 10,
	ARG_DT              = 1 << 11,
	ARG_HASH            = 1 << 12,
	ARG_CMDLINE_ARG     = 1 << 13,
};

/* match arg flags (for verbose output) */
enum
{
	INFO_MAGIC          = 1 <<  0,
	INFO_CMDLINE        = 1 <<  1,
	INFO_PAGESIZE       = 1 <<  2,
	INFO_BASE           = 1 <<  3,
	INFO_KERNEL_OFFSET  = 1 <<  4,
	INFO_RAMDISK_OFFSET = 1 <<  5,
	INFO_SECOND_OFFSET  = 1 <<  6,
	INFO_TAGS_OFFSET    = 1 <<  7,
	INFO_KERNEL_SIZE    = 1 <<  8,
	INFO_RAMDISK_SIZE   = 1 <<  9,
	INFO_SECOND_SIZE    = 1 << 10,
	INFO_DT_SIZE        = 1 << 11,
	INFO_HASH           = 1 << 12,
	// ARG_CMDLINE_ARG  = 1 << 13,
	INFO_ACTUALHASH     = 1 << 14,
};

void print_boot_info(boot_img *image, const unsigned info)
{
	char *hash;

	if (info & INFO_MAGIC)
		LOGV("BOARD_MAGIC \"%s\"", image->hdr.board);
	if (info & INFO_CMDLINE)
		LOGV("BOARD_CMDLINE \"%s\"", image->hdr.cmdline);
	if (info & INFO_PAGESIZE)
		LOGV("BOARD_PAGESIZE %u", image->hdr.pagesize);
	if (info & INFO_BASE)
		LOGV("BOARD_BASE 0x%08X", image->base);
	if (info & INFO_KERNEL_OFFSET)
		LOGV("BOARD_KERNEL_OFFSET 0x%08X", image->kernel_offset);
	if (info & INFO_RAMDISK_OFFSET)
		LOGV("BOARD_RAMDISK_OFFSET 0x%08X", image->ramdisk_offset);
	if (info & INFO_SECOND_OFFSET)
		LOGV("BOARD_SECOND_OFFSET 0x%08X", image->second_offset);
	if (info & INFO_TAGS_OFFSET)
		LOGV("BOARD_TAGS_OFFSET 0x%08X", image->tags_offset);
	if (info & INFO_KERNEL_SIZE)
		LOGV("BOARD_KERNEL_SIZE %u", image->hdr.kernel_size);
	if (info & INFO_RAMDISK_SIZE)
		LOGV("BOARD_RAMDISK_SIZE %u", image->hdr.ramdisk_size);
	if (info & INFO_SECOND_SIZE)
		LOGV("BOARD_SECOND_SIZE %u", image->hdr.second_size);
	if (info & INFO_DT_SIZE)
		LOGV("BOARD_DT_SIZE %u", image->hdr.dt_size);
	if (info & INFO_HASH) {
		hash = read_hash((byte*)image->hdr.hash);
		LOGV("BOARD_HASH 0x%s", hash);
		free(hash);
	}
	if (info & INFO_ACTUALHASH) {
		hash = read_hash(bootimg_generate_hash(image));
		LOGV("ACTUALHASH 0x%s", hash);
		free(hash);
	}
}

#define usage(...) { \
	LOGE("Usage: %s [xvcf] [args...]\n", argv[0]); \
	LOGE( \
		" Modes:\n" \
		"  -x, --unpack        - unpack an Android boot image\n" \
		"  -c, --create        - create an Android boot image\n" \
		"  -u, --update        - update an Android boot image\n" \
		"  -v, -vv, --verbose  - print boot image details\n" \
		"\n" \
		" Options: (set value only for create/update mode)\n" \
		"   [ -k,  --kernel \"kernel\"        ]\n" \
		"   [ -r,  --ramdisk \"ramdisk\"      ]\n" \
		"   [ -s,  --second \"second\"        ]\n" \
		"   [ -d,  --dt \"dt.img\"            ]\n" \
		"   [ -m,  --board \"board magic\"    ]\n" \
		"   [ -l,  --cmdline \"boot cmdline\" ]\n" \
		"   [ -a,  --arg \"cmdline\" \"value\"  ]\n" \
		"   [ -p,  --pagesize <size>        ]\n" \
		"   [ -b,  --base <hex>             ]\n" \
		"   [ -ko, --kernel_offset <hex>    ]\n" \
		"   [ -ro, --ramdisk_offset <hex>   ]\n" \
		"   [ -so, --second_offset <hex>    ]\n" \
		"   [ -to, --tags_offset <hex>      ]\n" \
		"   [ -h,  --hash                   ]\n" \
		" Unpack:\n" \
		"     -i,  --input \"boot.img\"\n" \
		"     -o,  --output \"directory\"\n" \
		" Create:\n" \
		"     -o,  --output \"boot.img\"\n" \
		"     -i,  --input \"directory\"\n" \
		" Update:\n" \
		"     -i,  --input \"boot.img\"\n" \
		"   [ -o,  --output \"boot.img\"      ]\n" \
	); \
	LOGE(__VA_ARGS__); \
	return EINVAL; \
}

#define specify(item) usage("You need to specify %s!", item)
#define failto(item, err) { LOGE("Failed to %s: %s", item, strerror(err)); return err; }
#define requireval { if (i > argc - 2) usage("%s requires a value in this mode!", argv[i]); }
#define foundfile(item) { if (verbose > 1) LOGV("Found %s: %s", item, file); }

int main(const int argc, const char** argv)
{
	boot_img *image;
	struct stat st = {0,};
	struct dirent *dp;
	DIR *dfd;
	char *bname, file[PATH_MAX], buf[1024], hex[16];
	const char *c,
		*input = 0, *output = 0,
		*kernel = 0, *ramdisk = 0, *second = 0,
		*dt = 0, *board = 0, *cmdline = 0;
	uint32_t base = 0,
		kernel_offset = 0, ramdisk_offset = 0,
		second_offset = 0, tags_offset = 0;
	int i, argstart,
		pagesize = 0, ret = 0,
		verbose = 0, mode = MODE_NONE;
	unsigned args = 0, info = 0;

	if (argc < 2)
		usage("Not enough arguments!");

	// tar style argument parsing (only valid in first arg)
	for (argstart = 2, c = argv[1]; *c; c++) {
		switch (*c) {
		case '-':
		case 'f':
			continue;
		case 'x':
			mode = MODE_UNPACK;
			continue;
		case 'c':
			mode = MODE_CREATE;
			continue;
		case 'u':
			mode = MODE_UPDATE;
			continue;
		case 'v':
			verbose++;
			continue;
		}
		mode = MODE_NONE;
		verbose = 0;
		argstart = 1;
		break;
	}

	// handle mode changes so next section can parse args in the right mode
	for (i = argstart; i < argc; i++) {
		if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "--unpack"))
			mode = MODE_UNPACK;
		else
		if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--create"))
			mode = MODE_CREATE;
		else
		if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--update"))
			mode = MODE_UPDATE;
		else
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose++;
		else
		if (!strcmp(argv[i], "-vv"))
			verbose += 2;
	}

	if (mode == MODE_NONE && verbose)
		mode = MODE_INFO;

	if (mode == MODE_NONE)
		specify("a mode of operation");

	for (i = argstart; i < argc; i++) {
		if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "--unpack")
		 || !strcmp(argv[i], "-c") || !strcmp(argv[i], "--create")
		 || !strcmp(argv[i], "-u") || !strcmp(argv[i], "--update")
		 || !strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			// do nothing, we already handled these args
		} else
		if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) {
			requireval;
			input = argv[++i];
		} else
		if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) {
			if (mode == MODE_INFO)
				usage("You can't use %s in info mode!", argv[i]);
			requireval;
			output = argv[++i];
		} else
		if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--kernel")) {
			args |= ARG_KERNEL;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				kernel = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--ramdisk")) {
			args |= ARG_RAMDISK;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				ramdisk = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--second")) {
			args |= ARG_SECOND;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				second = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dt")) {
			args |= ARG_DT;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				dt = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--magic")
		 || !strcmp(argv[i], "--board")) {
			args |= ARG_BOARD;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				board = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--cmdline")) {
			args |= ARG_CMDLINE;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				cmdline = argv[++i];
			}
		} else
		if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--pagesize")) {
			args |= ARG_PAGESIZE;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				pagesize = strtoul(argv[++i], 0, 10);
			}
		} else
		if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--base")) {
			args |= ARG_BASE;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				base = strtoul(argv[++i], 0, 16);
			}
		} else
		if (!strcmp(argv[i], "-ko")
		 || !strcmp(argv[i], "--kernel_offset")) {
			args |= ARG_KERNEL_OFFSET;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				kernel_offset = strtoul(argv[++i], 0, 16);
			}
		} else
		if (!strcmp(argv[i], "-ro")
		 || !strcmp(argv[i], "--ramdisk_offset")) {
			args |= ARG_RAMDISK_OFFSET;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				ramdisk_offset = strtoul(argv[++i], 0, 16);
			}
		} else
		if (!strcmp(argv[i], "-so")
		 || !strcmp(argv[i], "--second_offset")) {
			args |= ARG_SECOND_OFFSET;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				second_offset = strtoul(argv[++i], 0, 16);
			}
		} else
		if (!strcmp(argv[i], "-to")
		 || !strcmp(argv[i], "--tags_offset")) {
			args |= ARG_TAGS_OFFSET;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				requireval;
				tags_offset = strtoul(argv[++i], 0, 16);
			}
		} else
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--hash")) {
			args |= ARG_HASH;
		} else
		if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--arg")) {
			args |= ARG_CMDLINE_ARG;
			switch (mode) {
			case MODE_CREATE:
			case MODE_UPDATE:
				if (i > argc - 3)
					usage("%s requires both an argument and a value!", argv[i]);
				i += 2; // we should handle this in update section
				continue;
			}
			usage("You can only use %s in update/create mode!", argv[i]);
		} else
		{ // if it's not an argument, it's either an input or output
			switch (mode) {
			case MODE_CREATE:
				if (!output) {
					output = argv[i];
					continue;
				}
			case MODE_INFO:
			case MODE_UPDATE:
			case MODE_UNPACK:
				if (!input) {
					input = argv[i];
					continue;
				}
				if (mode == MODE_INFO)
					break;
				if (!output) {
					output = argv[i];
					continue;
				}
			}
			usage("Unknown argument: %s", argv[i]);
		}
	}

	if (verbose > 1)
		info = -1 - INFO_ACTUALHASH; // turn them all on!
	else
		info = args;

	switch (mode) {
	case MODE_UNPACK:
	case MODE_INFO:
		goto info;
	case MODE_CREATE:
		goto create;
	case MODE_UPDATE:
		goto update;
	}

	return 0; // this should never be reached

info:
	if (!input)
		specify("an input boot image");

	if (mode == MODE_UNPACK && !output)
		specify("an output directory");

	if (mode == MODE_UNPACK && !stat(output, &st) && !S_ISDIR(st.st_mode))
		failto("create output directory", ENOTDIR);

	image = load_boot_image(input);
	if (!image)
		failto("load boot image", EINVAL);

	if (mode == MODE_UNPACK && !S_ISDIR(st.st_mode)
		 && mkdir(output, NEW_DIR_PERMISSIONS))
		failto("create output directory", errno);

	if (!verbose)
		goto unpack;

	if (!info) {
		info = INFO_BASE | INFO_PAGESIZE | INFO_HASH
			| INFO_KERNEL_OFFSET | INFO_RAMDISK_OFFSET
			| INFO_SECOND_OFFSET | INFO_TAGS_OFFSET;

		if (*image->hdr.board)
			info |= INFO_MAGIC;
		if (*image->hdr.cmdline)
			info |= INFO_CMDLINE;
		if (image->kernel)
			info |= INFO_KERNEL_SIZE;
		if (image->ramdisk)
			info |= INFO_RAMDISK_SIZE;
		if (image->second)
			info |= INFO_SECOND_SIZE;
		if (image->dt)
			info |= INFO_DT_SIZE;
	}

	if (args & ARG_HASH)
		info |= INFO_ACTUALHASH;

	print_boot_info(image, info);

	if (mode == MODE_INFO)
		goto free;
	// otherwise continue to unpack

unpack:
	bname = basename(input);

	if (args & ARG_BOARD || !args) {
		sprintf(file, "%s/%s-board", output, bname);
		write_string_to_file(file, (char*)image->hdr.board);
	}
	if (args & ARG_CMDLINE || !args) {
		sprintf(file, "%s/%s-cmdline", output, bname);
		write_string_to_file(file, (char*)image->hdr.cmdline);
	}
	if (args & ARG_PAGESIZE || !args) {
		sprintf(file, "%s/%s-pagesize", output, bname);
		sprintf(hex, "%u", image->hdr.pagesize);
		write_string_to_file(file, hex);
	}
	if (args & ARG_BASE || !args) {
		sprintf(file, "%s/%s-base", output, bname);
		sprintf(hex, "%08X", image->base);
		write_string_to_file(file, hex);
	}
	if (args & ARG_KERNEL_OFFSET || !args) {
		sprintf(file, "%s/%s-kernel_offset", output, bname);
		sprintf(hex, "%08X", image->kernel_offset);
		write_string_to_file(file, hex);
	}
	if (args & ARG_RAMDISK_OFFSET || !args) {
		sprintf(file, "%s/%s-ramdisk_offset", output, bname);
		sprintf(hex, "%08X", image->ramdisk_offset);
		write_string_to_file(file, hex);
	}
	if (args & ARG_SECOND_OFFSET || !args) {
		sprintf(file, "%s/%s-second_offset", output, bname);
		sprintf(hex, "%08X", image->second_offset);
		write_string_to_file(file, hex);
	}
	if (args & ARG_TAGS_OFFSET || !args) {
		sprintf(file, "%s/%s-tags_offset", output, bname);
		sprintf(hex, "%08X", image->tags_offset);
		write_string_to_file(file, hex);
	}
	if (args & ARG_KERNEL || !args) {
		sprintf(file, "%s/%s-kernel", output, bname);
		bootimg_save_kernel(image, file);
	}
	if (args & ARG_RAMDISK || !args) {
		sprintf(file, "%s/%s-ramdisk", output, bname);
		bootimg_save_ramdisk(image, file);
	}
	if (args & ARG_SECOND || !args) {
		sprintf(file, "%s/%s-second", output, bname);
		bootimg_save_second(image, file);
	}
	if (args & ARG_DT || !args) {
		sprintf(file, "%s/%s-dt", output, bname);
		bootimg_save_dt(image, file);
	}

	free(bname);
	goto free;

create:
	if (!output)
		specify("an output boot image");

	image = new_boot_image();

	if (!input)
		goto modify;

	if (!(dfd = opendir(input)))
		failto("open boot image directory", errno);

	while ((dp = readdir(dfd))) {
		if (!(c = strrchr(dp->d_name, '-')))
			continue;
		sprintf(file, "%s/%s", input, dp->d_name);
		if (stat(file, &st) || !S_ISREG(st.st_mode))
			continue;
		c++;
		if (!(args & ARG_BOARD) && !strcmp(c, "board")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("board");
			if (!(board = strdup(buf)))
				continue;
			args |= ARG_BOARD;
			continue;
		}
		if (!(args & ARG_CMDLINE) && !strcmp(c, "cmdline")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("cmdline");
			if (!(cmdline = strdup(buf)))
				continue;
			args |= ARG_CMDLINE;
			continue;
		}
		if (!(args & ARG_PAGESIZE) && !strcmp(c, "pagesize")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("pagesize");
			pagesize = strtoul(buf, 0, 10);
			args |= ARG_PAGESIZE;
			continue;
		}
		if (!(args & ARG_BASE) && !strcmp(c, "base")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("base");
			base = strtoul(buf, 0, 16);
			args |= ARG_BASE;
			continue;
		}
		if (!(args & ARG_KERNEL_OFFSET) && !strcmp(c, "kernel_offset")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("kernel_offset");
			kernel_offset = strtoul(buf, 0, 16);
			args |= ARG_KERNEL_OFFSET;
			continue;
		}
		if (!(args & ARG_RAMDISK_OFFSET) && !strcmp(c, "ramdisk_offset")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("ramdisk_offset");
			ramdisk_offset = strtoul(buf, 0, 16);
			args |= ARG_RAMDISK_OFFSET;
			continue;
		}
		if (!(args & ARG_SECOND_OFFSET) && !strcmp(c, "second_offset")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("second_offset");
			second_offset = strtoul(buf, 0, 16);
			args |= ARG_SECOND_OFFSET;
			continue;
		}
		if (!(args & ARG_TAGS_OFFSET) && !strcmp(c, "tags_offset")) {
			if (read_string_from_file(file, buf, sizeof(buf)))
				continue;
			foundfile("tags_offset");
			tags_offset = strtoul(buf, 0, 16);
			args |= ARG_TAGS_OFFSET;
			continue;
		}
		if (!(args & ARG_KERNEL) && !strcmp(c, "kernel")) {
			foundfile("kernel");
			if (!(kernel = strdup(file)))
				continue;
			args |= ARG_KERNEL;
			continue;
		}
		if (!(args & ARG_RAMDISK) && !strcmp(c, "ramdisk")) {
			foundfile("ramdisk");
			if (!(ramdisk = strdup(file)))
				continue;
			args |= ARG_RAMDISK;
			continue;
		}
		if (!(args & ARG_SECOND) && !strcmp(c, "second")) {
			foundfile("second");
			if (!(second = strdup(file)))
				continue;
			args |= ARG_SECOND;
			continue;
		}
		if (!(args & ARG_DT) && !strcmp(c, "dt")) {
			foundfile("dt");
			if (!(dt = strdup(file)))
				continue;
			args |= ARG_DT;
			continue;
		}
	}

	closedir(dfd);

	info |= args;

	goto modify;

update:
	if (!input)
		specify("an input boot image");

	image = load_boot_image(input);
	if (!image)
		failto("load boot image", ENOENT);

	if (!output)
		output = input;

	// continue through modify
modify:
	if (args & ARG_BASE)
		bootimg_set_base(image, base);
	if (args & ARG_KERNEL_OFFSET)
		bootimg_set_kernel_offset(image, kernel_offset);
	if (args & ARG_RAMDISK_OFFSET)
		bootimg_set_ramdisk_offset(image, ramdisk_offset);
	if (args & ARG_SECOND_OFFSET)
		bootimg_set_second_offset(image, second_offset);
	if (args & ARG_TAGS_OFFSET)
		bootimg_set_tags_offset(image, tags_offset);

	if (args & ARG_PAGESIZE) {
		ret = bootimg_set_pagesize(image, pagesize);
		if (ret) {
			free_boot_image(image);
			failto("set page size", ret);
		}
	}

	if (args & ARG_BOARD) {
		ret = bootimg_set_board(image, board);
		if (ret) {
			free_boot_image(image);
			failto("set board magic", ret);
		}
	}

	if (args & ARG_CMDLINE) {
		ret = bootimg_set_cmdline(image, cmdline);
		if (ret) {
			free_boot_image(image);
			failto("set cmdline", ret);
		}
	}

	if (args & ARG_CMDLINE_ARG) {
		for (i = argstart; i < argc; i++) {
			if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--arg")) {
				i++;
				if (!strcmp(argv[i + 1], "-"))
					ret = bootimg_delete_cmdline_arg(image, argv[i]);
				else
					ret = bootimg_set_cmdline_arg(image, argv[i], argv[i + 1]);
				if (ret) {
					free_boot_image(image);
					failto("set cmdline argument", ret);
				}
				i++;
			}
		}
		info |= INFO_CMDLINE;
	}

	if (args & ARG_KERNEL) {
		ret = bootimg_load_kernel(image, kernel);
		if (ret) {
			free_boot_image(image);
			failto("load kernel image", ret);
		}
	}

	if (args & ARG_RAMDISK) {
		ret = bootimg_load_ramdisk(image, ramdisk);
		if (ret) {
			free_boot_image(image);
			failto("load ramdisk image", ret);
		}
	}

	if (args & ARG_SECOND) {
		ret = bootimg_load_second(image, second);
		if (ret) {
			free_boot_image(image);
			failto("load second image", ret);
		}
	}

	if (args & ARG_DT) {
		ret = bootimg_load_dt(image, dt);
		if (ret) {
			free_boot_image(image);
			failto("load dt image", ret);
		}
	}

	if (args & ARG_HASH) {
		bootimg_update_hash(image);
		info |= INFO_HASH;
	}

	if (verbose)
		print_boot_info(image, info);

	ret = write_boot_image(image, output);
	if (ret)
		failto("write boot image", ret);

free:
	free_boot_image(image);
	return ret;
}
