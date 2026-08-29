// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal built-in rescue shell for Sharp Brain Gen1 bring-up.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define LINE_SIZE 256
#define MAX_ARGS 16

static void make_dir(const char *path)
{
	if (mkdir(path, 0755) && errno != EEXIST)
		perror(path);
}

static void mount_fs(const char *source, const char *target, const char *type)
{
	make_dir(target);
	if (mount(source, target, type, 0, NULL) && errno != EBUSY)
		perror(target);
}

static int attach_console(const char *path)
{
	int fd;

	if (setsid() < 0 && errno != EPERM)
		return -1;
	fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -1;
	if (ioctl(fd, TIOCSCTTY, 1) < 0 && errno != EPERM) {
		close(fd);
		return -1;
	}
	if (tcsetpgrp(fd, getpgrp()) < 0 && errno != ENOTTY)
		perror("tcsetpgrp");
	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO)
		close(fd);
	return 0;
}

static void cat_file(const char *path)
{
	char buf[512];
	ssize_t count;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		perror(path);
		return;
	}
	while ((count = read(fd, buf, sizeof(buf))) > 0)
		if (write(STDOUT_FILENO, buf, count) != count)
			break;
	close(fd);
}

static void list_dir(const char *path)
{
	struct dirent *entry;
	DIR *dir = opendir(path ? path : ".");

	if (!dir) {
		perror(path ? path : ".");
		return;
	}
	while ((entry = readdir(dir)))
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			printf("%s\n", entry->d_name);
	closedir(dir);
}

static int split_line(char *line, char **argv)
{
	int argc = 0;
	char *word;
	char *save;

	for (word = strtok_r(line, " \t\r\n", &save);
	     word && argc < MAX_ARGS - 1;
	     word = strtok_r(NULL, " \t\r\n", &save))
		argv[argc++] = word;
	argv[argc] = NULL;
	return argc;
}

static void shell_loop(const char *console)
{
	char line[LINE_SIZE];
	char cwd[PATH_MAX];

	setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
	printf("\nSharp Brain Gen1 rescue shell on %s\n", console);
	printf("Type 'help' for built-in commands.\n");
	for (;;) {
		char *argv[MAX_ARGS];
		int argc;

		printf("brain# ");
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin)) {
			clearerr(stdin);
			continue;
		}
		argc = split_line(line, argv);
		if (!argc)
			continue;
		if (!strcmp(argv[0], "help")) {
			puts("help echo cat ls cd pwd uname clear");
		} else if (!strcmp(argv[0], "echo")) {
			int i;

			for (i = 1; i < argc; i++)
				printf("%s%s", i == 1 ? "" : " ", argv[i]);
			putchar('\n');
		} else if (!strcmp(argv[0], "cat") && argc == 2) {
			cat_file(argv[1]);
		} else if (!strcmp(argv[0], "ls")) {
			list_dir(argc == 2 ? argv[1] : NULL);
		} else if (!strcmp(argv[0], "cd") && argc == 2) {
			if (chdir(argv[1]))
				perror(argv[1]);
		} else if (!strcmp(argv[0], "pwd")) {
			puts(getcwd(cwd, sizeof(cwd)) ? cwd : "?");
		} else if (!strcmp(argv[0], "uname")) {
			struct utsname name;

			if (!uname(&name))
				printf("%s %s %s %s\n", name.sysname,
				       name.release,
				       name.machine, name.nodename);
		} else if (!strcmp(argv[0], "clear")) {
			fputs("\033[H\033[J", stdout);
		} else {
			printf("%s: command not found\n", argv[0]);
		}
	}
}

static int run_init(void)
{
	pid_t serial;

	mount_fs("devtmpfs", "/dev", "devtmpfs");
	mount_fs("proc", "/proc", "proc");
	mount_fs("sysfs", "/sys", "sysfs");
	make_dir("/dev/pts");
	if (mount("devpts", "/dev/pts", "devpts", 0, NULL) && errno != EBUSY)
		perror("/dev/pts");

	serial = fork();
	if (!serial) {
		if (!attach_console("/dev/ttyAMA0"))
			shell_loop("ttyAMA0");
		_exit(1);
	}
	if (attach_console("/dev/tty0")) {
		perror("/dev/tty0");
		if (serial > 0)
			waitpid(serial, NULL, 0);
		return 1;
	}
	shell_loop("tty0");
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = strrchr(argv[0], '/');

	(void)argc;
	name = name ? name + 1 : argv[0];
	if (!strcmp(name, "init"))
		return run_init();
	shell_loop("inherited tty");
	return 0;
}
