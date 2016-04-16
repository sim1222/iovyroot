/*
 * Copyright (C) 2016 @fi01
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <linux/loop.h>
#include "start_daemon.h"

#define SU_IMAGE_PATH           "/data/su.img"
#define SU_MOUNT_POINT          "/su"

#define INSMOD_CMD              "/system/bin/insmod"
#define MOUNT_CMD               "/system/bin/mount"
#define MKDIR_CMD               "/system/bin/mkdir"
#define BUSYBOX_CMD             "/data/local/busybox"

#define INIT_D_PATH             "/su/etc/init.d"
#define RIC_DISABLER_MOD_PATH   "/data/local/ric_disabler_mod.ko"


static int
set_init_ns(void)
{
  int fd;

  fd = open("/proc/1/ns/mnt", O_RDONLY);
  if (fd == -1) {
    if (errno == ENOENT) {
      return 0;
    }

    printf("Failed to open ns/mnt\n");
    return -1;
  }

  printf("    [+] Setup mount name space\n");

  if (setns(fd, 0) == -1) {
    printf("Failed to setns to ns/mnt\n");

    close(fd);
    return -1;
  }

  close(fd);

  fd = open("/proc/1/ns/net", O_RDONLY);
  if (fd == -1) {
    printf("Failed to open ns/net\n");
    return -1;
  }

  if (setns(fd, 0) == -1) {
    printf("Failed to setns to ns/net\n");

    close(fd);
    return -1;
  }

  close(fd);

  return 0;
}

int wait_exec(pid_t pid)
{
  int ret;

  printf("    [+] Wait for pid %d\n", pid);

  ret = waitpid(pid, NULL, 0);
  if (ret == -1) {
    perror("waitpid()");
    return -1;
  }

  return 0;
}

static int
run_command(char *const *cmd)
{
  pid_t pid;
  int ret;

  pid = fork();

  switch (pid) {
  case -1:
    perror("fork()");
    return -1;

  case 0:
    ret = execv(cmd[1], &cmd[1]);
    if (ret == -1) {
      int i;

      perror("execv()");

      for (i = 1; cmd[i]; i++)
        printf(" %s", cmd[i]);

      printf("\n");

      fflush(stdout);
    }

    exit(1);

  default:
    printf("    [+] Exec command: %s\n", cmd[0]);

    if (wait_exec(pid)) {
      return -1;
    }
  }

  return 0;
}

static int
make_mount_point(void)
{
  static char * const cmd1[] = {
    "Disable RIC",
    INSMOD_CMD,
    RIC_DISABLER_MOD_PATH,
    NULL
  };
  static char * const cmd2[] = {
    "Remount rw rootfs",
    MOUNT_CMD,
    "-o",
    "rw,remount",
    "/",
    NULL
  };
  static char * const cmd3[] = {
    "Make mount point for su.img",
    MKDIR_CMD,
    "-p",
    SU_MOUNT_POINT,
    NULL
  };

  if (run_command(cmd1)
   || run_command(cmd2)
   || run_command(cmd3)) {
    return -1;
  }

  return 0;
}

int
start_daemon(void)
{
  static char * const cmd[] = {
    "Run parts in " INIT_D_PATH,
    BUSYBOX_CMD,
    "run-parts",
    INIT_D_PATH,
    NULL
  };

  return run_command(cmd);
}

int
mount_su_image(void)
{
  const char *type = "ext4";
  const char *source = SU_IMAGE_PATH;
  const char *target = SU_MOUNT_POINT;
  unsigned flags = MS_RDONLY;
  const char *options = NULL;
  char loopname[64];
  int mode, loop, fd;
  struct loop_info64 info;
  int n;

  set_init_ns();

  if (make_mount_point()) {
    return -1;
  }

  printf("    [+] Setup loopback interface\n");

  mode = (flags & MS_RDONLY) ? O_RDONLY : O_RDWR;
  fd = open(source, mode | O_CLOEXEC);
  if (fd < 0) {
      return -1;
  }

  for (n = 0; ; n++) {
    snprintf(loopname, sizeof(loopname), "/dev/block/loop%d", n);
    loop = open(loopname, mode | O_CLOEXEC);
    if (loop < 0) {
      close(fd);
      return -1;
    }

    if (ioctl(loop, LOOP_GET_STATUS64, &info) >= 0) {
      if (strcmp((const char *)info.lo_file_name, source) == 0) {
        printf("    [+] Already mounted\n");

        close(fd);
        close(loop);
        return 0;
      }
    }
    else if (errno == ENXIO) {
      if (ioctl(loop, LOOP_SET_FD, fd) >= 0) {
        int ret;

        close(fd);

        memset(&info, 0, sizeof info);
        memcpy(info.lo_file_name, source, sizeof (info.lo_file_name) - 1);
        ioctl(loop, LOOP_SET_STATUS64, &info);

        printf("    [+] Use loopback device as %s\n", loopname);

        printf("    [+] Mount su image\n");

        ret = mount(loopname, target, type, flags, options);
        if (ret < 0) {
          ioctl(loop, LOOP_CLR_FD, 0);
          close(loop);
          return -1;
        }

        close(loop);
        return 0;
      }
    }

    close(loop);
  }

  return -1;
}
